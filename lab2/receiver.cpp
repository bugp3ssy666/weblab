// receiver.cpp
#include "protocol.h"
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <algorithm>
#include <string>
#include <iomanip>
#include <cstdio>

class Receiver {
private:
    SOCKET sockfd;
    struct sockaddr_in local_addr;
    struct sockaddr_in sender_addr;
    int sender_addr_len;
    ConnectionState state;

    // 接收缓冲管理
    uint32_t expected_seq;
    std::map<uint32_t, Packet> recv_buffer;
    std::set<uint32_t> received_seqs;

    // 输出文件和统计
    std::ofstream output_file;
    uint64_t total_bytes_received;
    uint64_t total_packets_received;

    // 连接管理
    bool client_locked;
    sockaddr_in client_addr;

    int transfer_counter;

public:
    Receiver(const char* bind_ip, uint16_t port) {
        sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sockfd == INVALID_SOCKET) {
            std::cerr << "创建套接字失败，错误码: " << WSAGetLastError() << std::endl;
            exit(1);
        }

        u_long mode = 1;
        ioctlsocket(sockfd, FIONBIO, &mode);

        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(port);
        local_addr.sin_addr.s_addr = inet_addr(bind_ip);

        if (local_addr.sin_addr.s_addr == INADDR_NONE) {
            std::cerr << "非法的服务器IP地址" << std::endl;
            exit(1);
        }

        if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR) {
            std::cerr << "绑定失败，错误码: " << WSAGetLastError() << std::endl;
            exit(1);
        }

        sender_addr_len = sizeof(sender_addr);
        state = CLOSED;
        expected_seq = 0;

        total_bytes_received = 0;
        total_packets_received = 0;

        client_locked = false;
        memset(&client_addr, 0, sizeof(client_addr));

        transfer_counter = 0;

        std::cout << "\n════════ 接收端已启动 ════════" << std::endl;
        std::cout << "监听端口: " << port << std::endl;
        std::cout << "等待连接中..." << std::endl;
    }

    ~Receiver() {
        if (sockfd != INVALID_SOCKET) {
            closesocket(sockfd);
        }
        if (output_file.is_open()) {
            output_file.close();
        }
    }

    void run() {
        while (true) {
            Packet packet;
            if (receive_packet(packet)) {
                if (!packet.verify_checksum()) {
                    std::cerr << "校验和错误，丢弃数据包" << std::endl;
                    continue;
                }

                handle_packet(packet);

                if (state == CLOSED) {
                    break;
                }

                if (++transfer_counter % 10 == 0 && state == ESTABLISHED) {
                    show_spinner();
                }
            }

            Sleep(1);
        }

        printf("\r \r");
        fflush(stdout);

        std::cout << "\n════════ 接收完成 ════════" << std::endl;
        std::cout << "──────────────────────────────" << std::endl;
        std::cout << "  总接收字节:  " << total_bytes_received << std::endl;
        std::cout << "  总接收包数:  " << total_packets_received << std::endl;
        std::cout << "──────────────────────────────" << std::endl;
    }

private:
    bool receive_packet(Packet& packet) {
        uint8_t buffer[MAX_PACKET_SIZE * 2];

        int recv_len = recvfrom(sockfd, reinterpret_cast<char*>(buffer),
                                sizeof(buffer), 0,
                                (struct sockaddr*)&sender_addr,
                                &sender_addr_len);

        if (recv_len > 0) {
            if (client_locked && !same_endpoint(sender_addr, client_addr)) {
                return false;
            }

            packet = Packet::deserialize(buffer, recv_len);
            total_packets_received++;
            return true;
        }

        return false;
    }

    void send_packet(const Packet& packet) {
        std::vector<uint8_t> buffer = packet.serialize();
        sendto(sockfd, reinterpret_cast<const char*>(buffer.data()),
               static_cast<int>(buffer.size()), 0,
               (struct sockaddr*)&sender_addr, sender_addr_len);
    }

    void show_spinner() {
        static int spin_state = 0;
        const char spinners[] = { '|', '/', '-', '\\' };
        printf("\r%c", spinners[spin_state % 4]);
        fflush(stdout);
        spin_state++;
    }

    void handle_packet(const Packet& packet) {
        switch (packet.header.type) {
            case SYN:
                handle_syn(packet);
                break;
            case FILE_NAME:
                handle_file_name(packet);
                break;
            case DATA:
                handle_data(packet);
                break;
            case FIN:
                handle_fin(packet);
                break;
            default:
                break;
        }
    }

    void handle_syn(const Packet& syn_packet) {
        if (!client_locked) {
            client_addr = sender_addr;
            client_locked = true;
            std::cout << "\n========== 连接建立 ==========" << std::endl;
            std::cout << "[✓] 已锁定客户端: " << inet_ntoa(client_addr.sin_addr)
                      << ":" << ntohs(client_addr.sin_port) << std::endl;
        }

        std::cout << "[✓] 收到SYN，建立连接" << std::endl;

        Packet syn_ack;
        syn_ack.header.type = SYN_ACK;
        syn_ack.header.ack_num = syn_packet.header.seq_num + 1;
        syn_ack.header.checksum = syn_ack.calculate_checksum();

        send_packet(syn_ack);

        expected_seq = syn_packet.header.seq_num + 1;
        state = ESTABLISHED;
    }

    void handle_data(const Packet& data_packet) {
        uint32_t seq = data_packet.header.seq_num;

        if (received_seqs.find(seq) == received_seqs.end()) {
            recv_buffer[seq] = data_packet;
            received_seqs.insert(seq);
            total_bytes_received += data_packet.header.data_length;
        }

        while (recv_buffer.find(expected_seq) != recv_buffer.end()) {
            const Packet& pkt = recv_buffer[expected_seq];
            output_file.write(reinterpret_cast<const char*>(pkt.data),
                            pkt.header.data_length);
            recv_buffer.erase(expected_seq);
            expected_seq++;
        }

        send_ack();
    }

    void handle_fin(const Packet& fin_packet) {
        std::cout << "\n========== 连接关闭 ==========" << std::endl;
        std::cout << "[✓] 收到FIN，关闭连接" << std::endl;

        Packet fin_ack;
        fin_ack.header.type = FIN_ACK;
        fin_ack.header.ack_num = fin_packet.header.seq_num + 1;
        fin_ack.header.checksum = fin_ack.calculate_checksum();

        send_packet(fin_ack);

        state = CLOSED;
        output_file.close();

        std::cout << "[✓] 连接已安全关闭！" << std::endl;
    }

    void handle_file_name(const Packet& name_packet) {
        if (name_packet.header.data_length > 0) {
            uint16_t len = name_packet.header.data_length;
            if (len > MAX_DATA_SIZE) len = MAX_DATA_SIZE;
            std::string orig(reinterpret_cast<const char*>(name_packet.data), len);

            size_t slash_pos = orig.find_last_of("/\\");
            std::string basename = (slash_pos != std::string::npos) ? 
                                   orig.substr(slash_pos + 1) : orig;

            std::string name_only, ext;
            size_t dot_pos = basename.find_last_of('.');
            if (dot_pos != std::string::npos) {
                name_only = basename.substr(0, dot_pos);
                ext = basename.substr(dot_pos);
            } else {
                name_only = basename;
                ext = std::string();
            }

            std::string output_name = name_only + "_output" + ext;
            std::cout << "\n========== 数据接收 ==========" << std::endl;
            output_file.open(output_name, std::ios::binary);
            if (!output_file.is_open()) {
                std::cerr << "[✗] 无法创建输出文件: " << output_name << std::endl;
            } else {
                std::cout << "[✓] 输出文件已创建: " << output_name << std::endl;
            }
        } else {
            std::cout << "\n========== 数据接收 ==========" << std::endl;
            std::cout << "[!] 收到空的文件名，使用默认 output 文件名" << std::endl;
            output_file.open("output", std::ios::binary);
        }

        // 发送FILE_NAME确认
        Packet file_name_ack;
        file_name_ack.header.type = FILE_NAME_ACK;
        file_name_ack.header.ack_num = name_packet.header.seq_num + 1;
        file_name_ack.header.checksum = file_name_ack.calculate_checksum();
        send_packet(file_name_ack);
        std::cout << "[✓] 已发送FILE_NAME确认" << std::endl;
    }

    void send_ack() {
        Packet ack_packet;
        ack_packet.header.type = ACK;
        ack_packet.header.ack_num = expected_seq;
        ack_packet.header.window_size = WINDOW_SIZE;

        std::vector<SACKBlock> sack_blocks;

        auto it = received_seqs.upper_bound(expected_seq);
        while (it != received_seqs.end() && sack_blocks.size() < 3) {
            uint32_t left = *it;
            uint32_t right = left + 1;

            auto next_it = it;
            ++next_it;
            while (next_it != received_seqs.end() && *next_it == right) {
                right++;
                ++next_it;
            }

            SACKBlock sack;
            sack.left_edge = left;
            sack.right_edge = right;
            sack_blocks.push_back(sack);

            it = next_it;
        }

        ack_packet.sack_blocks = sack_blocks;
        ack_packet.header.sack_count = static_cast<uint32_t>(sack_blocks.size());
        ack_packet.header.checksum = ack_packet.calculate_checksum();

        send_packet(ack_packet);
    }

    bool same_endpoint(const sockaddr_in& a, const sockaddr_in& b) {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }
};

int main(int argc, char* argv[]) {
    WinsockInitializer winsock;

    std::string bind_ip;
    uint16_t port;

    std::cout << "\n══════════ 接收端配置 ══════════" << std::endl;
    std::cout << "请输入绑定IP地址: ";
    std::cin >> bind_ip;
    std::cout << "请输入端口号: ";
    std::cin >> port;

    Receiver receiver(bind_ip.c_str(), port);
    receiver.run();

    std::cout << "按任意键退出..." << std::endl;
    std::cin.ignore();
    std::cin.get();

    return 0;
}
