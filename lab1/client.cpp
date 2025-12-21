// client.cpp
//
// MinGW:
//   g++ -std=c++17 client.cpp -lws2_32 -o client.exe
//
// Simple console client that sends LOGIN first, then listens for messages and allows user to type messages.
// Use /quit to exit client.

#include "chatroom.h"

std::atomic<bool> client_running(true);           // 客户端运行标志

// 接收线程函数：循环地接收服务器发送的消息并打印到控制台
void recv_thread(SOCKET s, const std::string& nickname){
    while (client_running){
        // 和服务端一样的接收与检查逻辑
        uint32_t len_be;
        int rec = recv(s, (char*)&len_be, 4, MSG_WAITALL);
        if (rec <= 0) break;
        uint32_t len = ntohl(len_be);       // 服务器字节序转为主机字节序，大端->小端
        if (len < 1) break;
        uint8_t type;
        rec = recv(s, (char*)&type, 1, MSG_WAITALL);
        if (rec <= 0) break;
        int payload_len = (int)len - 1;
        std::string payload;
        if (payload_len > 0) {
            payload.resize(payload_len);
            rec = recv(s, &payload[0], payload_len, MSG_WAITALL);
            if (rec <= 0) break;
        }
        // 根据消息类型打印不同的信息到 console
        if (type == SERVER_BROADCAST || type == SERVER_NOTICE) {
            std::cout << "\r" << std::string(nickname.size() + 2, ' ') << "\r";  // 清除当前行
            set_console_color(type == SERVER_BROADCAST ? COLOR_DEFAULT : COLOR_YELLOW);
            std::cout << payload << std::endl;
            set_console_color(COLOR_DEFAULT);
            set_console_color(COLOR_CYAN);
            std::cout << nickname << ": ";  // 重新显示输入提示符
            set_console_color(COLOR_DEFAULT);
            std::cout.flush();
        } else {
            // 未知类型，直接打印 raw 的 payload
            std::cout << "[unknown msg] " << payload << std::endl;
        }
    }
    // 退出接收循环后，标记客户端停止运行
    client_running = false;
}

int main(){
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 设置控制台模式
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(hStdin, &mode);
    mode &= ~ENABLE_QUICK_EDIT_MODE;
    mode |= ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT;
    SetConsoleMode(hStdin, mode);

    // 初始化 Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        set_console_color(COLOR_RED);
        std::cerr << "[ERROR] WSAStartup failed\n";
        set_console_color(COLOR_DEFAULT);
        return 1;
    }

    // 读取服务器 IP 和端口
    std::string srv_ip = "127.0.0.1";
    int port = 12345;
    // // alternative: allow user input
    // // disabled for now
    // std::cout << "Server IP (default 127.0.0.1): ";
    // std::string tmp; std::getline(std::cin, tmp); if (!tmp.empty()) srv_ip = tmp;
    // std::cout << "Port (default 12345): ";
    // std::getline(std::cin, tmp); if (!tmp.empty()) port = stoi(tmp);

    // 创建客户端 socket 并连接服务器
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        set_console_color(COLOR_RED);
        std::cerr << "[ERROR] socket failed\n";
        set_console_color(COLOR_DEFAULT);
        WSACleanup();
        return 1;
    }

    // 设置服务器地址结构
    sockaddr_in srv{};  
    srv.sin_family = AF_INET;                           // 设置地址族为 IPv4
    srv.sin_addr.s_addr = inet_addr(srv_ip.c_str());    // 设置服务器 IP 地址
    srv.sin_port = htons(port);                         // 设置服务器端口

    // 连接到服务器
    if (connect(sock, (sockaddr*)&srv, sizeof(srv)) == SOCKET_ERROR) {      // 调用基本 socket 连接函数，连接服务器
        int err = WSAGetLastError();
        set_console_color(COLOR_RED);
        std::cerr << "[ERROR] Failed to connect to server.\n";
        if (err == WSAECONNREFUSED) {
            std::cerr << "        Server is not running or refused the connection.\n";
        } else if (err == WSAETIMEDOUT) {
            std::cerr << "        Connection timed out.\n";
        } else {
            std::cerr << "        Error code: " << err << "\n";
        }
        set_console_color(COLOR_DEFAULT);
        std::cerr << "Press any key to exit...\n";
        _getch();
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 验证昵称是否合法
    auto is_valid_nickname = [](const std::string& name) -> bool {
        if (name.empty()) return false;
        
        for (size_t i = 0; i < name.size(); ) {
            unsigned char c = name[i];
            
            // ASCII 字符：英文字母、数字、下划线、连字符
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
                (c >= '0' && c <= '9') || c == '_' || c == '-') {
                i++;
            }
            // UTF-8 汉字范围（CJK统一汉字）
            else if (i + 2 < name.size() && c >= 0xE4 && c <= 0xE9) {
                unsigned char c2 = name[i + 1];
                unsigned char c3 = name[i + 2];
                
                // 验证是否在汉字范围内
                if (c == 0xE4 && c2 >= 0xB8 && c2 <= 0xBF) {
                    i += 3;
                } else if (c >= 0xE5 && c <= 0xE8 && c2 >= 0x80 && c2 <= 0xBF) {
                    i += 3;
                } else if (c == 0xE9 && c2 >= 0x80 && c2 <= 0xBF) {
                    i += 3;
                } else {
                    return false;
                }
            }
            else {
                return false;  // 非法字符
            }
        }
        return true;
    };

    // 获取用户在 console 键入的昵称
    std::cout << "Your nickname: ";
    std::string nickname; read_console_line(nickname);
    
    // 检查是否输入 /quit
    if (nickname == "/quit") {
        std::cout << "Client exited.\n";
        closesocket(sock);
        WSACleanup();
        return 0;
    }
    
    while (nickname.empty() || !is_valid_nickname(nickname)) {
        if (!nickname.empty()) {
            set_console_color(COLOR_RED);
            std::cout << "[ERROR] Invalid nickname. Only letters, numbers, Chinese characters, _ and - are allowed.\n";
            set_console_color(COLOR_DEFAULT);
        } else {
            set_console_color(COLOR_RED);
            std::cout << "[ERROR] Nickname cannot be empty.\n";
            set_console_color(COLOR_DEFAULT);
        }
        std::cout << "Your nickname: ";
        read_console_line(nickname);
        
        // 检查是否输入 /quit
        if (nickname == "/quit") {
            std::cout << "Client exited.\n";
            closesocket(sock);
            WSACleanup();
            return 0;
        }
    }

    // 发送 LOGIN 帧消息给服务器
    if (!send_frame(sock, CLIENT_LOGIN, nickname)) {
        set_console_color(COLOR_RED);
        std::cerr << "[ERROR] send login failed\n";
        set_console_color(COLOR_DEFAULT);
        closesocket(sock);
        WSACleanup();
        return 1;
    }


    // 启动接收线程
    std::thread rcv(recv_thread, sock, nickname);

    set_console_color(COLOR_GREEN);
    std::cout << "[CONNECTED] Type messages and press Enter to send. Type '/quit' to exit.\n";
    set_console_color(COLOR_DEFAULT);

    // console 循环
    // 监听用户在 console 输入的消息并发送给服务器
    std::string line;
    while (client_running){
        set_console_color(COLOR_CYAN);
        std::cout << nickname << ": ";
        set_console_color(COLOR_DEFAULT);
        std::cout.flush();
        
        if (!read_console_line(line)) {
            break;
        }
        
        if (line == "/quit") {
            send_frame(sock, CLIENT_LOGOUT, "");
            break;
        }
        if (!send_frame(sock, CLIENT_MSG, line)) {
            set_console_color(COLOR_RED);
            std::cerr << "[ERROR] send failed\n";
            set_console_color(COLOR_DEFAULT);
            break;
        }
    }

    // 退出客户端
    client_running = false;
    shutdown(sock, SD_BOTH);        // 关闭 socket 的发送和接收功能
    if (rcv.joinable()) rcv.join(); // 等待接收线程结束

    // 清理资源
    closesocket(sock);
    WSACleanup();
    set_console_color(COLOR_YELLOW);
    std::cout << "[TERMINATED] Client exited.\n";
    set_console_color(COLOR_DEFAULT);
    return 0;
}
