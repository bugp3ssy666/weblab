// protocol.h
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <iostream>

// 协议常量定义
const uint16_t MAX_DATA_SIZE = 1024;
const uint16_t HEADER_SIZE = 20;
const uint16_t MAX_PACKET_SIZE = MAX_DATA_SIZE + HEADER_SIZE;
const uint32_t WINDOW_SIZE = 16;
const uint32_t TIMEOUT_MS = 1000;

// 数据包类型
enum PacketType : uint8_t {
    SYN = 0x01,
    SYN_ACK = 0x02,
    DATA = 0x03,
    ACK = 0x04,
    FIN = 0x05,
    FIN_ACK = 0x06,
    FILE_NAME = 0x07
};

// 协议数据包头部结构
#pragma pack(push, 1)
struct PacketHeader {
    uint8_t type;
    uint8_t flags;
    uint16_t checksum;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t window_size;
    uint16_t data_length;
    uint32_t sack_count;

    PacketHeader() {
        memset(this, 0, sizeof(PacketHeader));
    }
};
#pragma pack(pop)

// SACK块结构
#pragma pack(push, 1)
struct SACKBlock {
    uint32_t left_edge;
    uint32_t right_edge;
};
#pragma pack(pop)

// 完整数据包结构
struct Packet {
    PacketHeader header;
    uint8_t data[MAX_DATA_SIZE];
    std::vector<SACKBlock> sack_blocks;

    Packet() {
        memset(data, 0, MAX_DATA_SIZE);
    }

    // 计算校验和 (反码求和)
    uint16_t calculate_checksum() const {
        uint32_t sum = 0;
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&header);

        // 头部校验和 (跳过checksum字段)
        sum += (ptr[0] << 8) | ptr[1];
        for (size_t i = 4; i < sizeof(PacketHeader); i += 2) {
            if (i + 1 < sizeof(PacketHeader)) {
                sum += (ptr[i] << 8) | ptr[i + 1];
            } else {
                sum += ptr[i] << 8;
            }
        }

        // 数据部分校验和
        for (uint16_t i = 0; i < header.data_length; i += 2) {
            if (i + 1 < header.data_length) {
                sum += (data[i] << 8) | data[i + 1];
            } else {
                sum += data[i] << 8;
            }
        }

        // SACK块校验和
        for (const auto& sack : sack_blocks) {
            const uint8_t* sack_ptr = reinterpret_cast<const uint8_t*>(&sack);
            for (size_t i = 0; i < sizeof(SACKBlock); i += 2) {
                sum += (sack_ptr[i] << 8) | sack_ptr[i + 1];
            }
        }

        // 处理进位
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }

        return static_cast<uint16_t>(~sum);
    }

    // 验证校验和
    bool verify_checksum() const {
        uint16_t original = header.checksum;
        const_cast<PacketHeader&>(header).checksum = 0;
        uint16_t calculated = calculate_checksum();
        const_cast<PacketHeader&>(header).checksum = original;
        return calculated == original;
    }

    // 序列化数据包
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;

        const uint8_t* header_ptr = reinterpret_cast<const uint8_t*>(&header);
        buffer.insert(buffer.end(), header_ptr, header_ptr + sizeof(PacketHeader));

        buffer.insert(buffer.end(), data, data + header.data_length);

        for (const auto& sack : sack_blocks) {
            const uint8_t* sack_ptr = reinterpret_cast<const uint8_t*>(&sack);
            buffer.insert(buffer.end(), sack_ptr, sack_ptr + sizeof(SACKBlock));
        }

        return buffer;
    }

    // 反序列化数据包
    static Packet deserialize(const uint8_t* buffer, size_t length) {
        Packet packet;

        if (length < sizeof(PacketHeader)) {
            return packet;
        }

        memcpy(&packet.header, buffer, sizeof(PacketHeader));
        size_t offset = sizeof(PacketHeader);

        // 反序列化数据
        if (packet.header.data_length > 0 &&
            offset + packet.header.data_length <= length) {
            memcpy(packet.data, buffer + offset, packet.header.data_length);
            offset += packet.header.data_length;
        }

        // 反序列化SACK块
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

// 连接状态
enum ConnectionState {
    CLOSED,
    SYN_SENT,
    ESTABLISHED,
    FIN_WAIT,
    CLOSE_WAIT
};

// 拥塞控制状态
enum CongestionState {
    SLOW_START,
    CONGESTION_AVOIDANCE,
    FAST_RECOVERY
};

// Windows Socket初始化类
class WinsockInitializer {
public:
    WinsockInitializer() {
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup失败，错误码: " << result << std::endl;
            exit(1);
        }
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    ~WinsockInitializer() {
        WSACleanup();
    }
};

#endif // PROTOCOL_H
