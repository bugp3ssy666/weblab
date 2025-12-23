// sender.cpp
// 文件说明: UDP可靠传输协议的发送端实现
// 功能: 实现文件的可靠传输，包括连接管理、数据发送、拥塞控制等

#include "protocol.h"
#include <iostream>
#include <fstream>
#include <map>
#include <chrono>
#include <algorithm>
#include <string>
#include <iomanip>
#include <cstdio>

// ==================== 发送端类 ====================
// 功能: 负责文件的可靠传输，实现滑动窗口、拥塞控制和重传机制
class Sender {
private:
    // ==================== 网络通信相关 ====================
    SOCKET sockfd;                      // UDP套接字描述符
    struct sockaddr_in receiver_addr;   // 接收端地址信息
    ConnectionState state;              // 当前连接状态

    // ==================== 序列号管理 ====================
    uint32_t seq_num;        // 初始序列号
    uint32_t base;           // 滑动窗口的基序列号(最小未确认序列号)
    uint32_t next_seq_num;   // 下一个要发送的序列号

    // ==================== 已发送包管理 ====================
    std::map<uint32_t, Packet> sent_packets;  // 已发送但未确认的数据包缓存
    std::map<uint32_t, std::chrono::steady_clock::time_point> send_times;  // 每个包的发送时间

    // ==================== SYN/FIN重传管理 ====================
    Packet syn_packet; 
    std::chrono::steady_clock::time_point syn_send_time;  // SYN包发送时间
    Packet fin_packet; 
    std::chrono::steady_clock::time_point fin_send_time;  // FIN包发送时间
    int syn_retries;    // SYN包重传次数
    int fin_retries;    // FIN包重传次数

    // ==================== 拥塞控制(TCP Reno算法) ====================
    CongestionState cong_state;  // 当前拥塞控制状态
    double cwnd;                 // 拥塞窗口大小(单位:数据包)
    uint32_t ssthresh;           // 慢启动阈值
    uint32_t duplicate_acks;     // 重复 ACK 计数器
    uint32_t last_acked;         // 最后一次确认的序列号

    // ==================== 统计信息 ====================
    uint64_t total_bytes_sent;    // 总发送字节数
    uint64_t total_packets_sent;  // 总发送包数
    uint64_t retransmissions;     // 重传次数

    // ==================== 连接管理 ====================
    sockaddr_in local_addr;   // 本地地址信息
    bool server_locked;       // 是否已锁定服务器(防止从其他地址接收数据)
    sockaddr_in server_addr;  // 锁定的服务器地址

    uint32_t receiver_window;  // 接收端窗口大小

public:
    // ==================== 构造函数 ====================
    // 功能: 初始化发送端，创建套接字并配置网络参数
    // 参数: sender_ip-本地IP, sender_port-本地端口, receiver_ip-接收端IP, receiver_port-接收端端口
    Sender(const char* sender_ip, uint16_t sender_port,
           const char* receiver_ip, uint16_t receiver_port) {
        // 1. 创建 UDP 套接字
        sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sockfd == INVALID_SOCKET) {
            std::cerr << "创建套接字失败，错误码: " << WSAGetLastError() << std::endl;
            exit(1);
        }

        // 2. 设置为非阻塞模式
        u_long mode = 1;
        ioctlsocket(sockfd, FIONBIO, &mode);

        // 3. 配置接收端地址信息
        memset(&receiver_addr, 0, sizeof(receiver_addr));
        receiver_addr.sin_family = AF_INET;
        receiver_addr.sin_port = htons(receiver_port);
        receiver_addr.sin_addr.s_addr = inet_addr(receiver_ip);

        // 4. 配置本地地址信息
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(sender_port);
        local_addr.sin_addr.s_addr = inet_addr(sender_ip);

        if (local_addr.sin_addr.s_addr == INADDR_NONE) {
            std::cerr << "无效的本机IP地址" << std::endl;
            exit(1);
        }

        // 5. 绑定本地地址(允许接收响应)
        if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR) {
            std::cerr << "sender bind 失败: " << WSAGetLastError() << std::endl;
            exit(1);
        }

        // 6. 初始化连接状态和序列号
        state = CLOSED;
        seq_num = 0;
        base = 0;
        next_seq_num = 0;

        // 7. 初始化拥塞控制参数
        cong_state = SLOW_START;  // 从慢启动开始
        cwnd = 1.0;               // 初始拥塞窗口为1
        ssthresh = WINDOW_SIZE;   // 阈值设为最大窗口大小
        duplicate_acks = 0;
        last_acked = 0;

        // 8. 初始化统计信息
        total_bytes_sent = 0;
        total_packets_sent = 0;
        retransmissions = 0;

        // 9. 初始化连接管理
        server_locked = false;
        memset(&server_addr, 0, sizeof(server_addr));
        syn_retries = 0;
        fin_retries = 0;
    }

    // ==================== 发送控制包方法 ====================
    // 功能: 发送控制类型的数据包(SYN/FIN/FILE_NAME等)
    // 参数: packet-要发送的数据包
    void send_control_packet(const Packet& packet) {
        send_packet(packet);
    }

    // ==================== 等待文件名确认方法 ====================
    // 功能: 发送文件名后等待接收端确认，超时重传
    // 参数: file_name_pkt-文件名数据包
    // 返回: true-成功接收确认，false-超时失败
    bool wait_for_file_name_ack(const Packet& file_name_pkt) {
        std::cout << "正在等待文件名确认..." << std::endl;
        auto send_time = std::chrono::steady_clock::now();
        int retries = 0;

        while (true) {
            auto now = std::chrono::steady_clock::now();

            // 检查是否超过最大重试次数
            if (retries >= 5) {
                std::cerr << "[✗] 文件名确认超时（已重试" << retries << "次）" << std::endl;
                return false;
            }

            // 检查是否超时，需要重传
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - send_time).count();
            if (elapsed > TIMEOUT_MS && retries < 5) {
                std::cout << "文件名确认超时，进行第" << (retries + 1) << "次重传" << std::endl;
                send_packet(file_name_pkt);  // 重传FILE_NAME包
                retries++;
                send_time = now;
            }

            // 尝试接收确认包
            Packet ack_packet;
            sockaddr_in from;
            if (receive_packet(ack_packet, from)) {
                if (ack_packet.header.type == FILE_NAME_ACK && ack_packet.verify_checksum()) {
                    std::cout << "[✓] 收到文件名确认，开始传输数据" << std::endl;
                    return true;
                }
            }

            Sleep(10);  // 短暂等待后再次检查
        }
    }

    // ==================== 析构函数 ====================
    // 功能: 清理资源，关闭套接字
    ~Sender() {
        if (sockfd != INVALID_SOCKET) {
            closesocket(sockfd);
        }
    }

    // ==================== 建立连接方法 ====================
    // 功能: 与接收端建立连接(类似TCP三次握手)
    // 返回: true-连接成功，false-连接失败
    // 流程: 1.发送SYN  2.等待SYN_ACK  3.发送ACK 4.连接建立
    bool connect() {
        std::cout << "\n========== 连接阶段 ==========" << std::endl;
        std::cout << "正在建立连接..." << std::endl;

        // 1. 构造并发送 SYN 包
        syn_packet.header.type = SYN;
        syn_packet.header.seq_num = seq_num;
        syn_packet.header.data_length = 0;
        syn_packet.header.checksum = htons(syn_packet.calculate_checksum());

        send_packet(syn_packet);
        syn_send_time = std::chrono::steady_clock::now();
        state = SYN_SENT;
        syn_retries = 0;

        // 2. 等待 SYN_ACK 响应
        while (true) {
            auto now = std::chrono::steady_clock::now();

            // 检查是否超过最大重试次数
            if (syn_retries >= 5) {
                std::cerr << "连接超时（已重试" << syn_retries << "次）" << std::endl;
                return false;
            }

            // 检查 SYN 包是否超时，需要重传
            auto syn_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - syn_send_time).count();
            if (syn_elapsed > TIMEOUT_MS) {
                std::cout << "SYN包超时，进行第" << (syn_retries + 1) << "次重传" << std::endl;
                send_packet(syn_packet);
                syn_send_time = now;
                syn_retries++;
            }

            // 尝试接收 SYN_ACK
            Packet recv_packet;
            sockaddr_in from;
            if (receive_packet(recv_packet, from)) {
                // 首次接收到响应时锁定服务器地址
                if (!server_locked) {
                    server_addr = from;
                    server_locked = true;
                    std::cout << "[✓] 已锁定服务器: " << inet_ntoa(server_addr.sin_addr)
                              << ":" << ntohs(server_addr.sin_port) << std::endl;
                }

                // 验证 SYN_ACK 包
                if (recv_packet.header.type == SYN_ACK && recv_packet.verify_checksum()) {
                    // 发送第三次握手的ACK
                    Packet ack_packet;
                    ack_packet.header.type = ACK;
                    ack_packet.header.seq_num = seq_num + 1;
                    ack_packet.header.ack_num = recv_packet.header.seq_num + 1;
                    ack_packet.header.data_length = 0;
                    ack_packet.header.checksum = htons(ack_packet.calculate_checksum());
                    
                    send_packet(ack_packet);
                    
                    state = ESTABLISHED;
                    seq_num++;         // 序列号递增
                    base = seq_num;
                    next_seq_num = seq_num;
                    std::cout << "[✓] 已发送第三次握手ACK" << std::endl;
                    std::cout << "[✓] 连接建立成功！" << std::endl;
                    return true;
                }
            }

            Sleep(10);  // 短暂等待后再次检查
        }
    }

    // ==================== 发送文件方法 ====================
    // 功能: 使用滑动窗口协议发送文件数据
    // 参数: filename-要发送的文件路径
    // 返回: true-发送成功，false-发送失败
    // 特点: 支持拥塞控制、自动重传、SACK选择性确认
    bool send_file(const char* filename) {
        // 1. 打开文件并读取内容
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[✗] 无法打开文件: " << filename << std::endl;
            return false;
        }

        // 计算文件大小
        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::cout << "\n========== 数据传输阶段 ==========" << std::endl;
        std::cout << "文件大小: " << file_size << " 字节" << std::endl;

        auto start_time = std::chrono::steady_clock::now();

        // 读取文件内容到内存
        std::vector<uint8_t> file_data(file_size);
        file.read(reinterpret_cast<char*>(file_data.data()), file_size);
        file.close();

        // 计算需要发送的总包数
        uint32_t total_packets = (file_size + MAX_DATA_SIZE - 1) / MAX_DATA_SIZE;

        int transfer_counter = 0;
        // 2. 滑动窗口协议主循环
        while (base < seq_num + total_packets) {
            // 计算当前窗口限制(取拥塞窗口和流量控制窗口的最小值)
            uint32_t window_limit = std::min<uint32_t>(
                static_cast<uint32_t>(cwnd),
                static_cast<uint32_t>(WINDOW_SIZE)
            );

            // 3. 在窗口允许的范围内发送数据包
            while (next_seq_num < base + window_limit &&
                   next_seq_num < seq_num + total_packets) {
                Packet packet;
                packet.header.type = DATA;
                packet.header.seq_num = next_seq_num;

                // 计算当前包的数据位置和大小
                size_t pkt_offset = (next_seq_num - seq_num) * MAX_DATA_SIZE;
                size_t pkt_size = std::min<size_t>(
                    static_cast<size_t>(MAX_DATA_SIZE),
                    static_cast<size_t>(file_size - pkt_offset)
                );

                // 填充数据并计算校验和
                memcpy(packet.data, file_data.data() + pkt_offset, pkt_size);
                packet.header.data_length = static_cast<uint16_t>(pkt_size);
                packet.header.checksum = htons(packet.calculate_checksum());

                // 发送包并记录发送信息
                send_packet(packet);
                sent_packets[next_seq_num] = packet;
                send_times[next_seq_num] = std::chrono::steady_clock::now();

                next_seq_num++;
            }

            // 4. 接收并处理 ACK
            Packet ack_packet;
            sockaddr_in from;
            if (receive_packet(ack_packet, from)) {
                if (ack_packet.header.type == ACK && ack_packet.verify_checksum()) {
                    handle_ack(ack_packet);
                }
            }

            // 5. 检查超时并重传
            check_timeout();

            // 6. 显示进度动画
            if (++transfer_counter % 10 == 0) {
                show_spinner();
            }

            Sleep(1);  // 避免CPU占用过高
        }

        printf("\r \r");
        fflush(stdout);

        // 7. 计算并显示传输统计信息
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

    // ==================== 断开连接方法 ====================
    // 功能: 与接收端断开连接(类似TCP四次挥手)
    // 流程: 1.发送FIN  2.等待FIN_ACK  3.连接关闭
    void disconnect() {
        std::cout << "\n========== 连接关闭阶段 ==========" << std::endl;
        std::cout << "正在关闭连接..." << std::endl;

        // 1. 构造并发送 FIN 包
        fin_packet.header.type = FIN;
        fin_packet.header.seq_num = next_seq_num;
        fin_packet.header.checksum = htons(fin_packet.calculate_checksum());

        send_packet(fin_packet);
        fin_send_time = std::chrono::steady_clock::now();
        state = FIN_WAIT;
        fin_retries = 0;

        // 2. 等待 FIN_ACK 响应
        while (true) {
            auto now = std::chrono::steady_clock::now();

            // 检查是否超过最大重试次数
            if (fin_retries >= 5) {
                std::cout << "关闭连接超时（已重试" << fin_retries << "次）" << std::endl;
                break;  // 即使超时也关闭连接
            }

            // 检查 FIN 包是否超时，需要重传
            auto fin_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - fin_send_time).count();
            if (fin_elapsed > TIMEOUT_MS) {
                std::cout << "FIN包超时，进行第" << (fin_retries + 1) << "次重传" << std::endl;
                send_packet(fin_packet);
                fin_send_time = now;
                fin_retries++;
            }

            // 尝试接收 FIN_ACK
            Packet recv_packet;
            sockaddr_in from;
            if (receive_packet(recv_packet, from)) {
                if (recv_packet.header.type == FIN_ACK && recv_packet.verify_checksum()) {
                    state = CLOSED;
                    std::cout << "[✓] 连接已安全关闭！" << std::endl;
                    break;
                }
            }

            Sleep(10);  // 短暂等待后再次检查
        }
    }

private:
    // ==================== 发送数据包方法 ====================
    // 功能: 通过UDP套接字发送数据包
    // 参数: packet-要发送的数据包
    void send_packet(const Packet& packet) {
        std::vector<uint8_t> buffer = packet.serialize();  // 序列化为字节流
        sendto(sockfd, reinterpret_cast<const char*>(buffer.data()),
               static_cast<int>(buffer.size()), 0,
               (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));

        // 更新统计信息
        total_packets_sent++;
        total_bytes_sent += buffer.size();
    }

    // ==================== 进度动画显示方法 ====================
    // 功能: 在控制台显示旋转动画，表示正在传输
    void show_spinner() {
        static int spin_state = 0;
        const char spinners[] = { '|', '/', '-', '\\' };  // 四种状态的旋转符号
        printf("\r%c", spinners[spin_state % 4]);
        fflush(stdout);
        spin_state++;
    }

    // ==================== 接收数据包方法 ====================
    // 功能: 从套接字接收数据包
    // 参数: packet-存储接收到的数据包, from_addr-发送方地址
    // 返回: true-成功接收，false-无数据
    bool receive_packet(Packet& packet, sockaddr_in& from_addr) {
        uint8_t buffer[MAX_PACKET_SIZE * 2];
        int from_len = sizeof(from_addr);

        int recv_len = recvfrom(sockfd,
            reinterpret_cast<char*>(buffer),
            sizeof(buffer), 0,
            (struct sockaddr*)&from_addr, &from_len);

        if (recv_len > 0) {
            // 检查是否来自锁定的服务器(安全特性)
            if (server_locked && !same_endpoint(from_addr, server_addr)) {
                return false;  // 忽略来自其他地址的数据
            }

            packet = Packet::deserialize(buffer, recv_len);  // 反序列化
            return true;
        }

        return false;  // 无数据可读(非阻塞模式)
    }

    // ==================== 处理ACK方法 ====================
    // 功能: 处理接收到的ACK包，实现TCP Reno拥塞控制
    // 参数: ack_packet-接收到的ACK数据包
    // 特点: 支持快速重传、慢启动、拥塞避免、快速恢复
    void handle_ack(const Packet& ack_packet) {
        uint32_t ack_num = ack_packet.header.ack_num;

        // 情况 1: 接收到新的ACK(确认了新数据)
        if (ack_num > base) {
            base = ack_num;  // 移动窗口基序列号
            duplicate_acks = 0;  // 重置重复ACK计数器

            // 根据当前拥塞控制状态调整cwnd
            if (cong_state == SLOW_START) {
                // 慢启动: 指数增长
                cwnd += 1.0;
                if (cwnd >= ssthresh) {
                    cong_state = CONGESTION_AVOIDANCE;  // 达到阈值，转为拥塞避免
                }
            } else if (cong_state == CONGESTION_AVOIDANCE) {
                // 拥塞避免: 线性增长
                cwnd += 1.0 / cwnd;
            } else if (cong_state == FAST_RECOVERY) {
                // 快速恢复: 恢复为拥塞避免状态
                cwnd = ssthresh;
                cong_state = CONGESTION_AVOIDANCE;
            }

            // 移除已确认的数据包
            auto it = sent_packets.begin();
            while (it != sent_packets.end() && it->first < base) {
                send_times.erase(it->first);
                it = sent_packets.erase(it);
            }

            last_acked = ack_num;

        } else if (ack_num == last_acked) {
            // 情况 2: 接收到重复ACK(可能丢包)
            duplicate_acks++;

            // 快速重传: 接收到3个重复ACK
            if (duplicate_acks == 2) {
                if (sent_packets.find(ack_num) != sent_packets.end()) {
                    send_packet(sent_packets[ack_num]);  // 重传丢失的包
                    retransmissions++;
                    // 调整拥塞参数
                    ssthresh = (std::max)(static_cast<uint32_t>(cwnd / 2), 2u);
                    cwnd = ssthresh + 3;
                    cong_state = FAST_RECOVERY;
                }
            } else if (duplicate_acks > 2 && cong_state == FAST_RECOVERY) {
                // 快速恢复期间，每个额外的重复ACK增加cwnd
                cwnd += 1.0;
            }
        }

        // 处理SACK块(选择性确认)
        for (const auto& sack : ack_packet.sack_blocks) {
            for (uint32_t seq = sack.left_edge; seq < sack.right_edge; ++seq) {
                sent_packets.erase(seq);  // 移除已SACK确认的数据
                send_times.erase(seq);
            }
        }
    }

    // ==================== 检查超时方法 ====================
    // 功能: 检查是否有数据包超时未确认，并进行重传
    // 特点: 超时重传会触发拥塞控制调整(返回慢启动)
    void check_timeout() {
        auto now = std::chrono::steady_clock::now();

        for (auto it = send_times.begin(); it != send_times.end(); ) {
            // 计算已经过去的时间
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second).count();

            // 检查是否超过超时限制
            if (elapsed > TIMEOUT_MS) {
                uint32_t seq = it->first;

                // 重传超时的数据包
                if (sent_packets.find(seq) != sent_packets.end()) {
                    send_packet(sent_packets[seq]);
                    retransmissions++;
                    it->second = now;  // 更新发送时间

                    // 超时重传触发拥塞控制调整
                    ssthresh = (std::max)(static_cast<uint32_t>(cwnd / 2), 2u);
                    cwnd = 1.0;  // 重置拥塞窗口
                    cong_state = SLOW_START;  // 返回慢启动状态
                    duplicate_acks = 0;
                }

                ++it;
            } else {
                ++it;
            }
        }
    }

    // ==================== 比较地址方法 ====================
    // 功能: 比较两个网络地址是否相同
    // 参数: a, b-要比较的两个地址
    // 返回: true-地址相同，false-地址不同
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
    name_pkt.header.checksum = htons(name_pkt.calculate_checksum());
    sender.send_control_packet(name_pkt);

    if (!sender.wait_for_file_name_ack(name_pkt)) {
        std::cerr << "[✗] 文件名确认失败，程序退出" << std::endl;
        return 1;
    }

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
