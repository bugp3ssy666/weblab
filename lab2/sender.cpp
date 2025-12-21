// sender.cpp
#include "protocol.h"
#include <iostream>
#include <fstream>
#include <map>
#include <chrono>
#include <algorithm>
#include <string>
#include <iomanip>
#include <cstdio>

class Sender {
private:
    SOCKET sockfd;
    struct sockaddr_in receiver_addr;
    ConnectionState state;

    // 序列号管理
    uint32_t seq_num;
    uint32_t base;
    uint32_t next_seq_num;

    // 已发送包管理
    std::map<uint32_t, Packet> sent_packets;
    std::map<uint32_t, std::chrono::steady_clock::time_point> send_times;

    // SYN/FIN重传管理
    Packet syn_packet;
    std::chrono::steady_clock::time_point syn_send_time;
    Packet fin_packet;
    std::chrono::steady_clock::time_point fin_send_time;
    int syn_retries;
    int fin_retries;

    // 拥塞控制 (RENO算法)
    CongestionState cong_state;
    double cwnd;
    uint32_t ssthresh;
    uint32_t duplicate_acks;
    uint32_t last_acked;

    // 统计信息
    uint64_t total_bytes_sent;
    uint64_t total_packets_sent;
    uint64_t retransmissions;

    // 连接管理
    sockaddr_in local_addr;
    bool server_locked;
    sockaddr_in server_addr;

public:
    Sender(const char* sender_ip, uint16_t sender_port,
           const char* receiver_ip, uint16_t receiver_port) {
        sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sockfd == INVALID_SOCKET) {
            std::cerr << "创建套接字失败，错误码: " << WSAGetLastError() << std::endl;
            exit(1);
        }

        u_long mode = 1;
        ioctlsocket(sockfd, FIONBIO, &mode);

        memset(&receiver_addr, 0, sizeof(receiver_addr));
        receiver_addr.sin_family = AF_INET;
        receiver_addr.sin_port = htons(receiver_port);
        receiver_addr.sin_addr.s_addr = inet_addr(receiver_ip);

        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(sender_port);
        local_addr.sin_addr.s_addr = inet_addr(sender_ip);

        if (local_addr.sin_addr.s_addr == INADDR_NONE) {
            std::cerr << "无效的本机IP地址" << std::endl;
            exit(1);
        }

        if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR) {
            std::cerr << "sender bind 失败: " << WSAGetLastError() << std::endl;
            exit(1);
        }

        state = CLOSED;
        seq_num = 0;
        base = 0;
        next_seq_num = 0;

        cong_state = SLOW_START;
        cwnd = 1.0;
        ssthresh = WINDOW_SIZE;
        duplicate_acks = 0;
        last_acked = 0;

        total_bytes_sent = 0;
        total_packets_sent = 0;
        retransmissions = 0;

        server_locked = false;
        memset(&server_addr, 0, sizeof(server_addr));
        syn_retries = 0;
        fin_retries = 0;
    }

    void send_control_packet(const Packet& packet) {
        send_packet(packet);
    }

    ~Sender() {
        if (sockfd != INVALID_SOCKET) {
            closesocket(sockfd);
        }
    }

    bool connect() {
        std::cout << "\n========== 连接阶段 ==========" << std::endl;
        std::cout << "正在建立连接..." << std::endl;

        syn_packet.header.type = SYN;
        syn_packet.header.seq_num = seq_num;
        syn_packet.header.data_length = 0;
        syn_packet.header.checksum = syn_packet.calculate_checksum();

        send_packet(syn_packet);
        syn_send_time = std::chrono::steady_clock::now();
        state = SYN_SENT;
        syn_retries = 0;

        while (true) {
            auto now = std::chrono::steady_clock::now();

            if (syn_retries >= 5) {
                std::cerr << "连接超时（已重试" << syn_retries << "次）" << std::endl;
                return false;
            }

            auto syn_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - syn_send_time).count();
            if (syn_elapsed > TIMEOUT_MS) {
                std::cout << "SYN包超时，进行第" << (syn_retries + 1) << "次重传" << std::endl;
                send_packet(syn_packet);
                syn_send_time = now;
                syn_retries++;
            }

            Packet recv_packet;
            sockaddr_in from;
            if (receive_packet(recv_packet, from)) {
                if (!server_locked) {
                    server_addr = from;
                    server_locked = true;
                    std::cout << "[✓] 已锁定服务器: " << inet_ntoa(server_addr.sin_addr)
                              << ":" << ntohs(server_addr.sin_port) << std::endl;
                }

                if (recv_packet.header.type == SYN_ACK && recv_packet.verify_checksum()) {
                    state = ESTABLISHED;
                    seq_num++;
                    base = seq_num;
                    next_seq_num = seq_num;
                    std::cout << "[✓] 连接建立成功！" << std::endl;
                    return true;
                }
            }

            Sleep(10);
        }
    }

    bool send_file(const char* filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[✗] 无法打开文件: " << filename << std::endl;
            return false;
        }

        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::cout << "\n========== 数据传输阶段 ==========" << std::endl;
        std::cout << "文件大小: " << file_size << " 字节" << std::endl;

        auto start_time = std::chrono::steady_clock::now();

        std::vector<uint8_t> file_data(file_size);
        file.read(reinterpret_cast<char*>(file_data.data()), file_size);
        file.close();

        uint32_t total_packets = (file_size + MAX_DATA_SIZE - 1) / MAX_DATA_SIZE;

        int transfer_counter = 0;
        while (base < seq_num + total_packets) {
            uint32_t window_limit = std::min<uint32_t>(
                static_cast<uint32_t>(cwnd),
                static_cast<uint32_t>(WINDOW_SIZE)
            );

            while (next_seq_num < base + window_limit &&
                   next_seq_num < seq_num + total_packets) {
                Packet packet;
                packet.header.type = DATA;
                packet.header.seq_num = next_seq_num;

                size_t pkt_offset = (next_seq_num - seq_num) * MAX_DATA_SIZE;
                size_t pkt_size = std::min<size_t>(
                    static_cast<size_t>(MAX_DATA_SIZE),
                    static_cast<size_t>(file_size - pkt_offset)
                );

                memcpy(packet.data, file_data.data() + pkt_offset, pkt_size);
                packet.header.data_length = static_cast<uint16_t>(pkt_size);
                packet.header.checksum = packet.calculate_checksum();

                send_packet(packet);
                sent_packets[next_seq_num] = packet;
                send_times[next_seq_num] = std::chrono::steady_clock::now();

                next_seq_num++;
            }

            Packet ack_packet;
            sockaddr_in from;
            if (receive_packet(ack_packet, from)) {
                if (ack_packet.header.type == ACK && ack_packet.verify_checksum()) {
                    handle_ack(ack_packet);
                }
            }

            check_timeout();

            if (++transfer_counter % 10 == 0) {
                show_spinner();
            }

            Sleep(1);
        }

        printf("\r \r");
        fflush(stdout);

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        double throughput = (file_size * 8.0) / (duration / 1000.0) / 1024.0 / 1024.0;

        std::cout << "\n========== 传输统计 ==========" << std::endl;
        std::cout << "[✓] 传输完成！" << std::endl;
        std::cout << "──────────────────────────────" << std::endl;
        std::cout << "  传输时间:    " << duration << " ms" << std::endl;
        std::cout << "  吞吐率:      " << std::fixed << std::setprecision(2) << throughput << " Mbps" << std::endl;
        std::cout << "  总字节数:    " << total_bytes_sent << std::endl;
        std::cout << "  总包数:      " << total_packets_sent << std::endl;
        std::cout << "  重传次数:    " << retransmissions << std::endl;
        std::cout << "──────────────────────────────" << std::endl;

        return true;
    }

    void disconnect() {
        std::cout << "\n========== 连接关闭阶段 ==========" << std::endl;
        std::cout << "正在关闭连接..." << std::endl;

        fin_packet.header.type = FIN;
        fin_packet.header.seq_num = next_seq_num;
        fin_packet.header.checksum = fin_packet.calculate_checksum();

        send_packet(fin_packet);
        fin_send_time = std::chrono::steady_clock::now();
        state = FIN_WAIT;
        fin_retries = 0;

        while (true) {
            auto now = std::chrono::steady_clock::now();

            if (fin_retries >= 5) {
                std::cout << "关闭连接超时（已重试" << fin_retries << "次）" << std::endl;
                break;
            }

            auto fin_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - fin_send_time).count();
            if (fin_elapsed > TIMEOUT_MS) {
                std::cout << "FIN包超时，进行第" << (fin_retries + 1) << "次重传" << std::endl;
                send_packet(fin_packet);
                fin_send_time = now;
                fin_retries++;
            }

            Packet recv_packet;
            sockaddr_in from;
            if (receive_packet(recv_packet, from)) {
                if (recv_packet.header.type == FIN_ACK && recv_packet.verify_checksum()) {
                    state = CLOSED;
                    std::cout << "[✓] 连接已安全关闭！" << std::endl;
                    break;
                }
            }

            Sleep(10);
        }
    }

private:
    void send_packet(const Packet& packet) {
        std::vector<uint8_t> buffer = packet.serialize();
        sendto(sockfd, reinterpret_cast<const char*>(buffer.data()),
               static_cast<int>(buffer.size()), 0,
               (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));

        total_packets_sent++;
        total_bytes_sent += buffer.size();
    }

    void show_spinner() {
        static int spin_state = 0;
        const char spinners[] = { '|', '/', '-', '\\' };
        printf("\r%c", spinners[spin_state % 4]);
        fflush(stdout);
        spin_state++;
    }

    bool receive_packet(Packet& packet, sockaddr_in& from_addr) {
        uint8_t buffer[MAX_PACKET_SIZE * 2];
        int from_len = sizeof(from_addr);

        int recv_len = recvfrom(sockfd,
            reinterpret_cast<char*>(buffer),
            sizeof(buffer), 0,
            (struct sockaddr*)&from_addr, &from_len);

        if (recv_len > 0) {
            if (server_locked && !same_endpoint(from_addr, server_addr)) {
                return false;
            }

            packet = Packet::deserialize(buffer, recv_len);
            return true;
        }

        return false;
    }

    void handle_ack(const Packet& ack_packet) {
        uint32_t ack_num = ack_packet.header.ack_num;

        if (ack_num > base) {
            base = ack_num;
            duplicate_acks = 0;

            if (cong_state == SLOW_START) {
                cwnd += 1.0;
                if (cwnd >= ssthresh) {
                    cong_state = CONGESTION_AVOIDANCE;
                }
            } else if (cong_state == CONGESTION_AVOIDANCE) {
                cwnd += 1.0 / cwnd;
            } else if (cong_state == FAST_RECOVERY) {
                cwnd = ssthresh;
                cong_state = CONGESTION_AVOIDANCE;
            }

            auto it = sent_packets.begin();
            while (it != sent_packets.end() && it->first < base) {
                send_times.erase(it->first);
                it = sent_packets.erase(it);
            }

            last_acked = ack_num;

        } else if (ack_num == last_acked) {
            duplicate_acks++;

            if (duplicate_acks == 3) {
                if (sent_packets.find(ack_num) != sent_packets.end()) {
                    send_packet(sent_packets[ack_num]);
                    retransmissions++;
                    ssthresh = (std::max)(static_cast<uint32_t>(cwnd / 2), 2u);
                    cwnd = ssthresh + 3;
                    cong_state = FAST_RECOVERY;
                }
            } else if (duplicate_acks > 3 && cong_state == FAST_RECOVERY) {
                cwnd += 1.0;
            }
        }

        for (const auto& sack : ack_packet.sack_blocks) {
            for (uint32_t seq = sack.left_edge; seq < sack.right_edge; ++seq) {
                sent_packets.erase(seq);
                send_times.erase(seq);
            }
        }
    }

    void check_timeout() {
        auto now = std::chrono::steady_clock::now();

        for (auto it = send_times.begin(); it != send_times.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second).count();

            if (elapsed > TIMEOUT_MS) {
                uint32_t seq = it->first;

                if (sent_packets.find(seq) != sent_packets.end()) {
                    send_packet(sent_packets[seq]);
                    retransmissions++;
                    it->second = now;

                    ssthresh = (std::max)(static_cast<uint32_t>(cwnd / 2), 2u);
                    cwnd = 1.0;
                    cong_state = SLOW_START;
                    duplicate_acks = 0;
                }

                ++it;
            } else {
                ++it;
            }
        }
    }

    bool same_endpoint(const sockaddr_in& a, const sockaddr_in& b) {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }
};

int main(int argc, char* argv[]) {
    WinsockInitializer winsock;

    std::string sender_ip, receiver_ip, filename;
    uint16_t sender_port, receiver_port;

    std::cout << "\n══════════ 发送端配置 ══════════" << std::endl;
    std::cout << "请输入本机IP地址: ";
    std::cin >> sender_ip;
    std::cout << "请输入本机端口号: ";
    std::cin >> sender_port;
    std::cout << "请输入接收端IP地址: ";
    std::cin >> receiver_ip;
    std::cout << "请输入接收端端口号: ";
    std::cin >> receiver_port;

    Sender sender(sender_ip.c_str(), sender_port, receiver_ip.c_str(), receiver_port);

    if (!sender.connect()) {
        std::cerr << "[✗] 连接失败，程序退出" << std::endl;
        return 1;
    }

    std::cout << "\n请输入要传输的文件路径: ";
    std::cin >> filename;

    std::ifstream file_check(filename, std::ios::binary);
    if (!file_check.is_open()) {
        std::cerr << "[✗] 无法打开文件: " << filename << std::endl;
        return 1;
    }
    file_check.close();

    std::string basename;
    size_t slash_pos = filename.find_last_of("/\\");
    if (slash_pos != std::string::npos) {
        basename = filename.substr(slash_pos + 1);
    } else {
        basename = filename;
    }

    Packet name_pkt;
    name_pkt.header.type = FILE_NAME;
    if (!basename.empty()) {
        uint16_t len = static_cast<uint16_t>(basename.size());
        if (len > MAX_DATA_SIZE) len = MAX_DATA_SIZE;
        name_pkt.header.data_length = len;
        memcpy(name_pkt.data, basename.data(), len);
    } else {
        name_pkt.header.data_length = 0;
    }
    name_pkt.header.checksum = name_pkt.calculate_checksum();
    sender.send_control_packet(name_pkt);

    if (!sender.send_file(filename.c_str())) {
        std::cerr << "[✗] 发送文件失败" << std::endl;
        return 1;
    }

    sender.disconnect();

    std::cout << "按任意键退出..." << std::endl;
    std::cin.ignore();
    std::cin.get();

    return 0;
}
