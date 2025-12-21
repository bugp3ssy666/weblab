#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <thread>
#include <vector>
#include <mutex>
#include <string>
#include <atomic>
#include <conio.h>

// 列举消息类型
enum MsgType : uint8_t {
    // 用户消息类型 0x0..
    CLIENT_LOGIN   = 0x01,
    CLIENT_MSG     = 0x02,
    CLIENT_LOGOUT  = 0x03,
    // 服务器消息类型 0x1..
    SERVER_BROADCAST = 0x11,
    SERVER_NOTICE    = 0x12,
    SERVER_LOGIN_REJECT = 0x13,
};

// 服务器端保存客户端信息结构体
struct ClientInfo {
    SOCKET sock;
    std::string nickname; // UTF-8
};

// ***一些工具函数
// 将宽字符转换为 UTF-8
std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

// 从控制台读取一行宽字符并转换为 UTF-8 字符串
bool read_console_line(std::string& output) {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t buffer[1024];
    DWORD read;
    
    if (!ReadConsoleW(hStdin, buffer, 1024, &read, NULL)) {
        return false;
    }
    
    // 移除换行符
    while (read > 0 && (buffer[read-1] == L'\n' || buffer[read-1] == L'\r')) {
        read--;
    }
    
    std::wstring wstr(buffer, read);
    output = wstring_to_utf8(wstr);
    return true;
}

// 设置控制台文本颜色
void set_console_color(WORD color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

// 定义颜色常量
#define COLOR_RED (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COLOR_GREEN (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_YELLOW (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_CYAN (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define COLOR_DEFAULT (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)


//===================用户端和服务端共用的方法函数==================//


// 流方式发送全部数据（被 send_frame 调用，实际用来发送完整数据帧）
bool send_all(SOCKET s, const char* buf, int len) {
    int sent = 0;
    // 循环检查机制：多次发送直到全部数据发送完毕
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0); // 调用基本 socket 发送函数
        if (n == SOCKET_ERROR) return false;    // 发送失败
        sent += n;
    }
    return true;
}

// 构造并发送一帧数据
// 帧格式：4 字节长度信息 + 1 字节 type 信息 + payload
// 只需要传入 type 和 payload，函数会自动构造完整帧并发送
bool send_frame(SOCKET s, uint8_t type, const std::string& payload){
    uint32_t len = 1 + (uint32_t)payload.size();    // len = type(1 byte) + payload size
    uint32_t len_be = htonl(len);                   // 转为大端序用于网络传输
    // 构建 buffer
    // buffer构成：4字节长度信息（已转换为大端） + 1字节type + payload
    std::string buf;
    buf.resize(4 + len);
    memcpy(&buf[0], &len_be, 4);
    buf[4] = (char)type;
    // 拷贝到缓冲区，并调用 send_all 发送数据帧
    if (!payload.empty()) memcpy(&buf[5], payload.data(), payload.size());
    return send_all(s, buf.data(), (int)buf.size());
}