// protocol.h
// 文件说明: UDP可靠传输协议头文件
// 功能: 定义了基于UDP的可靠传输协议的数据结构、常量和工具类
// 包含: 数据包结构、协议常量、连接状态、拥塞控制状态等定义

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <winsock2.h>     // Windows Socket API
#include <ws2tcpip.h>     // TCP/IP相关定义
#include <windows.h>      // Windows系统API
#include <cstdint>        // 标准整型定义
#include <cstring>        // C字符串操作
#include <vector>         // STL向量容器
#include <iostream>       // 标准输入输出流

// ==================== 协议常量定义 ====================
// 这些常量定义了协议的基本参数
const uint16_t MAX_DATA_SIZE = 1024;           // 单个数据包的最大数据负载大小(字节)
const uint16_t HEADER_SIZE = 20;               // 数据包头部固定大小(字节)
const uint16_t MAX_PACKET_SIZE = MAX_DATA_SIZE + HEADER_SIZE;  // 完整数据包最大大小
const uint32_t WINDOW_SIZE = 16;               // 滑动窗口大小(数据包个数)
const uint32_t TIMEOUT_MS = 1000;              // 超时重传时间(毫秒)

// ==================== 数据包类型枚举 ====================
// 定义了协议中使用的所有数据包类型
enum PacketType : uint8_t {
    SYN = 0x01,           // 同步包，用于建立连接(类似TCP三次握手)
    SYN_ACK = 0x02,       // 同步确认包，响应SYN请求
    DATA = 0x03,          // 数据包，传输实际文件内容
    ACK = 0x04,           // 确认包，确认接收到的数据
    FIN = 0x05,           // 结束包，请求关闭连接
    FIN_ACK = 0x06,       // 结束确认包，确认关闭请求
    FILE_NAME = 0x07,     // 文件名包，传输文件名信息
    FILE_NAME_ACK = 0x08  // 文件名确认包，确认接收到文件名
};

// ==================== 协议数据包头部结构 ====================
// 定义了每个数据包的头部信息结构
#pragma pack(push, 1)  // 设置1字节对齐，确保结构体紧凑存储
struct PacketHeader {
    uint8_t type;         // 数据包类型(SYN/ACK/DATA等)
    uint8_t flags;        // 标志位(预留字段，可用于扩展功能)
    uint16_t checksum;    // 校验和，用于检测数据传输错误
    uint32_t seq_num;     // 序列号，标识数据包的顺序
    uint32_t ack_num;     // 确认号，表示期望接收的下一个序列号
    uint16_t window_size; // 接收窗口大小，流量控制使用
    uint16_t data_length; // 数据部分的实际长度(字节)
    uint32_t sack_count;  // SACK块的数量(选择性确认)

    // 构造函数: 初始化所有字段为0
    PacketHeader() {
        memset(this, 0, sizeof(PacketHeader));
    }
};
#pragma pack(pop)

// ==================== SACK块结构 ====================
// 选择性确认(Selective Acknowledgment)块，用于告知发送方哪些数据已接收
#pragma pack(push, 1)
struct SACKBlock {
    uint32_t left_edge;   // SACK块的左边界(包含)，表示连续接收数据的起始序列号
    uint32_t right_edge;  // SACK块的右边界(不包含)，表示连续接收数据的结束序列号
};
#pragma pack(pop)

// ==================== 完整数据包结构 ====================
// 包含头部、数据负载和SACK信息的完整数据包
struct Packet {
    PacketHeader header;                 // 数据包头部
    uint8_t data[MAX_DATA_SIZE];         // 数据负载区域
    std::vector<SACKBlock> sack_blocks;  // SACK块列表，用于选择性确认

    // 构造函数: 初始化数据区域为0
    Packet() {
        memset(data, 0, MAX_DATA_SIZE);
    }

    // ==================== 校验和计算方法 ====================
    // 功能: 计算数据包的校验和，使用反码求和算法(类似TCP/IP校验和)
    // 返回: 16位校验和值
    // 说明: 包括头部、数据部分和SACK块
    uint16_t calculate_checksum() const {
        uint32_t sum = 0;  // 使用32位累加器防止溢出
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&header);

        // 1. 计算头部校验和
        for (size_t i = 0; i < sizeof(PacketHeader); i += 2) {
            if (i + 1 < sizeof(PacketHeader)) {
                sum += (ptr[i] << 8) | ptr[i + 1];  // 按16位字累加
            } else {
                sum += ptr[i] << 8;  // 处理奇数字节
            }
        }

        // 2. 计算数据部分校验和
        for (uint16_t i = 0; i < header.data_length; i += 2) {
            if (i + 1 < header.data_length) {
                sum += (data[i] << 8) | data[i + 1];  // 按16位字累加
            } else {
                sum += data[i] << 8;  // 处理最后一个奇数字节
            }
        }

        // 3. 计算SACK块校验和
        for (const auto& sack : sack_blocks) {
            const uint8_t* sack_ptr = reinterpret_cast<const uint8_t*>(&sack);
            for (size_t i = 0; i < sizeof(SACKBlock); i += 2) {
                sum += (sack_ptr[i] << 8) | sack_ptr[i + 1];
            }
        }

        // 4. 处理进位，将高16位加到低16位(反码求和的关键步骤)
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }

        // 5. 返回反码(按位取反)
        return static_cast<uint16_t>(~sum);
    }

    // ==================== 校验和验证方法 ====================
    // 功能: 验证接收到的数据包校验和是否正确
    // 返回: true-校验和正确，false-数据包损坏
    bool verify_checksum() const {
        // 校验和正确时结果应为全1，复用计算函数因为取反应为全0
        return calculate_checksum() == 0x0000;
    }

    // ==================== 数据包序列化方法 ====================
    // 功能: 将数据包结构转换为字节流，用于网络传输
    // 返回: 包含完整数据包的字节数组
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;

        // 1. 添加头部数据
        const uint8_t* header_ptr = reinterpret_cast<const uint8_t*>(&header);
        buffer.insert(buffer.end(), header_ptr, header_ptr + sizeof(PacketHeader));

        // 2. 添加数据负载(只添加有效数据)
        buffer.insert(buffer.end(), data, data + header.data_length);

        // 3. 添加SACK块
        for (const auto& sack : sack_blocks) {
            const uint8_t* sack_ptr = reinterpret_cast<const uint8_t*>(&sack);
            buffer.insert(buffer.end(), sack_ptr, sack_ptr + sizeof(SACKBlock));
        }

        return buffer;
    }

    // ==================== 数据包反序列化方法 ====================
    // 功能: 从字节流还原数据包结构
    // 参数: buffer-字节数组指针，length-数组长度
    // 返回: 反序列化后的数据包对象
    static Packet deserialize(const uint8_t* buffer, size_t length) {
        Packet packet;

        // 检查缓冲区长度是否足够包含头部
        if (length < sizeof(PacketHeader)) {
            return packet;  // 返回空包
        }

        // 1. 反序列化头部
        memcpy(&packet.header, buffer, sizeof(PacketHeader));
        size_t offset = sizeof(PacketHeader);

        // 2. 反序列化数据部分
        if (packet.header.data_length > 0 &&
            offset + packet.header.data_length <= length) {
            memcpy(packet.data, buffer + offset, packet.header.data_length);
            offset += packet.header.data_length;
        }

        // 3. 反序列化SACK块
        for (uint32_t i = 0; i < packet.header.sack_count; ++i) {
            if (offset + sizeof(SACKBlock) <= length) {
                SACKBlock sack;
                memcpy(&sack, buffer + offset, sizeof(SACKBlock));
                packet.sack_blocks.push_back(sack);
                offset += sizeof(SACKBlock);
            }
        }

        return packet;
    }
};

// ==================== 连接状态枚举 ====================
// 定义了连接生命周期中的各个状态(类似TCP状态机)
enum ConnectionState {
    CLOSED,        // 连接关闭状态，初始状态或连接终止后的状态
    SYN_SENT,      // 已发送SYN包，等待SYN_ACK响应
    SYN_RECEIVED,  // 已发送SYN_ACK，等待第三次握手ACK
    ESTABLISHED,   // 连接已建立，可以传输数据
    FIN_WAIT,      // 已发送FIN包，等待FIN_ACK响应
    CLOSE_WAIT     // 接收到FIN包，等待关闭(预留状态)
};

// ==================== 拥塞控制状态枚举 ====================
// 定义了TCP Reno拥塞控制算法的三个状态
enum CongestionState {
    SLOW_START,           // 慢启动阶段: cwnd指数增长
    CONGESTION_AVOIDANCE, // 拥塞避免阶段: cwnd线性增长
    FAST_RECOVERY         // 快速恢复阶段: 检测到丢包后的恢复
};

// ==================== Windows Socket初始化类 ====================
// RAII封装: 自动管理Winsock库的初始化和清理
// 使用方法: 在main函数开始处创建对象，程序结束时自动清理
class WinsockInitializer {
public:
    // 构造函数: 初始化Winsock 2.2并设置控制台UTF-8编码
    WinsockInitializer() {
        WSADATA wsaData;
        // 请求Winsock 2.2版本
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup失败，错误码: " << result << std::endl;
            exit(1);
        }
        // 设置控制台编码为UTF-8，支持中文显示
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    // 析构函数: 清理Winsock资源
    ~WinsockInitializer() {
        WSACleanup();
    }
};

#endif // PROTOCOL_H
