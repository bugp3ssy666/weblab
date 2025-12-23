// receiver.cpp
// 文件说明: UDP可靠传输协议的接收端实现
// 功能: 接收文件数据，实现乱序重组、去重、选择性确认等

#include "protocol.h"
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <algorithm>
#include <string>
#include <iomanip>
#include <cstdio>

// ==================== 接收端类 ====================
// 功能: 接收并保存发送端传输的文件，处理乱序数据包
class Receiver {
private:
    // ==================== 网络通信相关 ====================
    SOCKET sockfd;                      // UDP套接字描述符
    struct sockaddr_in local_addr;      // 本地绑定地址
    struct sockaddr_in sender_addr;     // 发送端地址信息
    int sender_addr_len;                // 发送端地址长度
    ConnectionState state;              // 当前连接状态

    // ==================== 接收缓冲管理 ====================
    uint32_t expected_seq;                           // 期望接收的下一个序列号
    std::map<uint32_t, Packet> recv_buffer;          // 接收缓冲区(存储乱序到达的包)
    std::set<uint32_t> received_seqs;                // 已接收序列号集合(用于去重和SACK)

    // ==================== 输出文件和统计 ====================
    std::ofstream output_file;          // 输出文件流
    uint64_t total_bytes_received;      // 总接收字节数
    uint64_t total_packets_received;    // 总接收包数

    // ==================== 连接管理 ====================
    bool client_locked;                 // 是否已锁定客户端(防止从其他地址接收数据)
    sockaddr_in client_addr;            // 锁定的客户端地址

    int transfer_counter;               // 传输计数器(用于控制动画显示)

public:
    // ==================== 构造函数 ====================
    // 功能: 初始化接收端，创建套接字并绑定端口
    // 参数: bind_ip-绑定的IP地址, port-监听端口
    Receiver(const char* bind_ip, uint16_t port) {
        // 1. 创建 UDP 套接字
        sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sockfd == INVALID_SOCKET) {
            std::cerr << "创建套接字失败，错误码: " << WSAGetLastError() << std::endl;
            exit(1);
        }

        // 2. 设置为非阻塞模式
        u_long mode = 1;
        ioctlsocket(sockfd, FIONBIO, &mode);

        // 3. 配置本地地址信息
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(port);
        local_addr.sin_addr.s_addr = inet_addr(bind_ip);

        if (local_addr.sin_addr.s_addr == INADDR_NONE) {
            std::cerr << "非法的服务器IP地址" << std::endl;
            exit(1);
        }

        // 4. 绑定套接字到本地地址
        if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR) {
            std::cerr << "绑定失败，错误码: " << WSAGetLastError() << std::endl;
            exit(1);
        }

        // 5. 初始化连接状态和序列号
        sender_addr_len = sizeof(sender_addr);
        state = CLOSED;
        expected_seq = 0;

        // 6. 初始化统计信息
        total_bytes_received = 0;
        total_packets_received = 0;

        // 7. 初始化连接管理
        client_locked = false;
        memset(&client_addr, 0, sizeof(client_addr));

        transfer_counter = 0;

        std::cout << "\n════════ 接收端已启动 ════════" << std::endl;
        std::cout << "监听端口: " << port << std::endl;
        std::cout << "等待连接中..." << std::endl;
    }

    // ==================== 析构函数 ====================
    // 功能: 清理资源，关闭套接字和文件
    ~Receiver() {
        if (sockfd != INVALID_SOCKET) {
            closesocket(sockfd);
        }
        if (output_file.is_open()) {
            output_file.close();
        }
    }

    // ==================== 主运行循环 ====================
    // 功能: 接收并处理数据包，直到连接关闭
    void run() {
        while (true) {
            Packet packet;
            // 1. 尝试接收数据包
            if (receive_packet(packet)) {
                // 2. 验证数据包校验和
                if (!packet.verify_checksum()) {
                    std::cerr << "校验和错误，丢弃数据包" << std::endl;
                    continue;
                }

                // 3. 根据包类型分发处理
                handle_packet(packet);

                // 4. 检查是否关闭连接
                if (state == CLOSED) {
                    break;
                }

                // 5. 定期显示进度动画
                if (++transfer_counter % 10 == 0 && state == ESTABLISHED) {
                    show_spinner();
                }
            }

            Sleep(1);  // 避免CPU占用过高
        }

        // 6. 清除进度动画
        printf("\r \r");
        fflush(stdout);

        // 7. 显示最终统计信息
        std::cout << "\n════════ 接收完成 ════════" << std::endl;
        std::cout << "──────────────────────────────" << std::endl;
        std::cout << "  总接收字节:  " << total_bytes_received << std::endl;
        std::cout << "  总接收包数:  " << total_packets_received << std::endl;
        std::cout << "──────────────────────────────" << std::endl;
    }

private:
    // ==================== 接收数据包方法 ====================
    // 功能: 从套接字接收数据包
    // 参数: packet-存储接收到的数据包
    // 返回: true-成功接收，false-无数据
    bool receive_packet(Packet& packet) {
        uint8_t buffer[MAX_PACKET_SIZE * 2];

        int recv_len = recvfrom(sockfd, reinterpret_cast<char*>(buffer),
                                sizeof(buffer), 0,
                                (struct sockaddr*)&sender_addr,
                                &sender_addr_len);

        if (recv_len > 0) {
            // 检查是否来自锁定的客户端(安全特性)
            if (client_locked && !same_endpoint(sender_addr, client_addr)) {
                return false;  // 忽略来自其他地址的数据
            }

            packet = Packet::deserialize(buffer, recv_len);  // 反序列化
            total_packets_received++;
            return true;
        }

        return false;  // 无数据可读(非阻塞模式)
    }

    // ==================== 发送数据包方法 ====================
    // 功能: 发送响应包(如ACK/SYN_ACK/FIN_ACK)
    // 参数: packet-要发送的数据包
    void send_packet(const Packet& packet) {
        std::vector<uint8_t> buffer = packet.serialize();  // 序列化为字节流
        sendto(sockfd, reinterpret_cast<const char*>(buffer.data()),
               static_cast<int>(buffer.size()), 0,
               (struct sockaddr*)&sender_addr, sender_addr_len);
    }

    // ==================== 进度动画显示方法 ====================
    // 功能: 在控制台显示旋转动画，表示正在接收
    void show_spinner() {
        static int spin_state = 0;
        const char spinners[] = { '|', '/', '-', '\\' };  // 四种状态的旋转符号
        printf("\r%c", spinners[spin_state % 4]);
        fflush(stdout);
        spin_state++;
    }

    // ==================== 数据包分发处理方法 ====================
    // 功能: 根据数据包类型调用相应的处理函数
    // 参数: packet-接收到的数据包
    void handle_packet(const Packet& packet) {
        switch (packet.header.type) {
            case SYN:        // 握手包
                handle_syn(packet);
                break;
            case ACK:        // 确认包(可能是第三次握手)
                handle_ack_handshake(packet);
                break;
            case FILE_NAME:  // 文件名包
                handle_file_name(packet);
                break;
            case DATA:       // 数据包
                handle_data(packet);
                break;
            case FIN:        // 结束包
                handle_fin(packet);
                break;
            default:
                break;
        }
    }

    // ==================== 处理第三次握手ACK方法 ====================
    // 功能: 处理第三次握手的ACK包，完成连接建立
    // 参数: ack_packet-接收到的ACK包
    void handle_ack_handshake(const Packet& ack_packet) {
        // 只在SYN_RECEIVED状态下处理握手ACK
        if (state == SYN_RECEIVED) {
            // 验证ACK序列号是否正确
            if (ack_packet.header.ack_num == 1) {  // 期望确认服务端的序列号0+1
                state = ESTABLISHED;
                std::cout << "[✓] 收到第三次握手ACK，连接正式建立！" << std::endl;
            } else {
                std::cout << "[!] 收到无效的握手ACK，序列号不匹配" << std::endl;
            }
        }
        // 如果已经是ESTABLISHED状态，这个ACK可能是数据传输的ACK，这里不处理
    }

    // ==================== 处理SYN包方法 ====================
    // 功能: 处理连接建立请求，响应SYN_ACK
    // 参数: syn_packet-接收到的SYN包
    void handle_syn(const Packet& syn_packet) {
        // 1. 首次接收到SYN包时，锁定客户端地址
        if (!client_locked) {
            client_addr = sender_addr;
            client_locked = true;
            std::cout << "\n========== 连接建立 ==========" << std::endl;
            std::cout << "[✓] 已锁定客户端: " << inet_ntoa(client_addr.sin_addr)
                      << ":" << ntohs(client_addr.sin_port) << std::endl;
        }

        std::cout << "[✓] 收到SYN，建立连接" << std::endl;

        // 2. 构造并发送 SYN_ACK 响应
        Packet syn_ack;
        syn_ack.header.type = SYN_ACK;
        syn_ack.header.seq_num = 0;  // 服务端的初始序列号
        syn_ack.header.ack_num = syn_packet.header.seq_num + 1;
        syn_ack.header.checksum = htons(syn_ack.calculate_checksum());

        send_packet(syn_ack);

        // 3. 更新状态和序列号，等待第三次握手
        expected_seq = syn_packet.header.seq_num + 1;
        state = SYN_RECEIVED;  // 进入SYN_RECEIVED状态，等待ACK
    }

    // ==================== 处理数据包方法 ====================
    // 功能: 接收数据包，实现乱序重组和去重
    // 参数: data_packet-接收到的数据包
    void handle_data(const Packet& data_packet) {
        // 确保连接已建立才处理数据包
        if (state != ESTABLISHED) {
            std::cout << "[!] 连接未建立，忽略数据包" << std::endl;
            return;
        }
        
        uint32_t seq = data_packet.header.seq_num;

        // 1. 检查是否为重复数据(去重)
        if (received_seqs.find(seq) == received_seqs.end()) {
            // 2. 存储新接收的数据包
            recv_buffer[seq] = data_packet;
            received_seqs.insert(seq);
            total_bytes_received += data_packet.header.data_length;
        }

        // 3. 乱序重组: 将连续的数据写入文件
        while (recv_buffer.find(expected_seq) != recv_buffer.end()) {
            const Packet& pkt = recv_buffer[expected_seq];
            output_file.write(reinterpret_cast<const char*>(pkt.data),
                            pkt.header.data_length);
            recv_buffer.erase(expected_seq);  // 移除已处理的包
            expected_seq++;  // 更新期望序列号
        }

        // 4. 发送ACK确认
        send_ack();
    }

    // ==================== 处理FIN包方法 ====================
    // 功能: 处理连接关闭请求，响应FIN_ACK
    // 参数: fin_packet-接收到的FIN包
    void handle_fin(const Packet& fin_packet) {
        std::cout << "\n========== 连接关闭 ==========" << std::endl;
        std::cout << "[✓] 收到FIN，关闭连接" << std::endl;

        // 1. 构造并发送 FIN_ACK 响应
        Packet fin_ack;
        fin_ack.header.type = FIN_ACK;
        fin_ack.header.ack_num = fin_packet.header.seq_num + 1;
        fin_ack.header.checksum = htons(fin_ack.calculate_checksum());

        send_packet(fin_ack);

        // 2. 关闭连接和文件
        state = CLOSED;
        output_file.close();

        std::cout << "[✓] 连接已安全关闭！" << std::endl;
    }

    // ==================== 处理文件名包方法 ====================
    // 功能: 接收文件名并创建输出文件
    // 参数: name_packet-包含文件名的数据包
    void handle_file_name(const Packet& name_packet) {
        // 确保连接已建立才处理文件名包
        if (state != ESTABLISHED) {
            std::cout << "[!] 连接未建立，忽略文件名包" << std::endl;
            return;
        }
        
        if (name_packet.header.data_length > 0) {
            // 1. 提取文件名
            uint16_t len = name_packet.header.data_length;
            if (len > MAX_DATA_SIZE) len = MAX_DATA_SIZE;
            std::string orig(reinterpret_cast<const char*>(name_packet.data), len);

            // 2. 提取文件基名(移除路径)
            size_t slash_pos = orig.find_last_of("/\\");
            std::string basename = (slash_pos != std::string::npos) ? 
                                   orig.substr(slash_pos + 1) : orig;

            // 3. 拆分文件名和扩展名
            std::string name_only, ext;
            size_t dot_pos = basename.find_last_of('.');
            if (dot_pos != std::string::npos) {
                name_only = basename.substr(0, dot_pos);
                ext = basename.substr(dot_pos);
            } else {
                name_only = basename;
                ext = std::string();
            }

            // 4. 构造输出文件名(添加"_output"后缀)
            std::string output_name = name_only + "_output" + ext;
            std::cout << "\n========== 数据接收 ==========" << std::endl;
            output_file.open(output_name, std::ios::binary);
            if (!output_file.is_open()) {
                std::cerr << "[✗] 无法创建输出文件: " << output_name << std::endl;
            } else {
                std::cout << "[✓] 输出文件已创建: " << output_name << std::endl;
            }
        } else {
            // 5. 如果没有文件名，使用默认名
            std::cout << "\n========== 数据接收 ==========" << std::endl;
            std::cout << "[!] 收到空的文件名，使用默认 output 文件名" << std::endl;
            output_file.open("output", std::ios::binary);
        }

        // 6. 发送文件名确认
        Packet file_name_ack;
        file_name_ack.header.type = FILE_NAME_ACK;
        file_name_ack.header.ack_num = name_packet.header.seq_num + 1;
        file_name_ack.header.checksum = htons(file_name_ack.calculate_checksum());
        send_packet(file_name_ack);
        std::cout << "[✓] 已发送FILE_NAME确认" << std::endl;
    }

    // ==================== 发送ACK确认方法 ====================
    // 功能: 发送带SACK信息的ACK确认包
    // 特点: 支持选择性确认(SACK)，告知发送方哪些乱序包已接收
    void send_ack() {
        Packet ack_packet;
        ack_packet.header.type = ACK;
        ack_packet.header.ack_num = expected_seq;  // 期望接收的下一个序列号
        ack_packet.header.window_size = WINDOW_SIZE;

        std::vector<SACKBlock> sack_blocks;

        // 构造SACK块: 找出所有乱序到达的连续区间
        auto it = received_seqs.upper_bound(expected_seq);  // 找到第一个大于expected_seq的序列号
        while (it != received_seqs.end() && sack_blocks.size() < 3) {  // 最多3个SACK块
            uint32_t left = *it;
            uint32_t right = left + 1;

            // 找到连续区间的右边界
            auto next_it = it;
            ++next_it;
            while (next_it != received_seqs.end() && *next_it == right) {
                right++;
                ++next_it;
            }

            // 添加SACK块
            SACKBlock sack;
            sack.left_edge = left;
            sack.right_edge = right;
            sack_blocks.push_back(sack);

            it = next_it;
        }

        // 设置SACK信息并计算校验和
        ack_packet.sack_blocks = sack_blocks;
        ack_packet.header.sack_count = static_cast<uint32_t>(sack_blocks.size());
        ack_packet.header.checksum = htons(ack_packet.calculate_checksum());

        send_packet(ack_packet);
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
