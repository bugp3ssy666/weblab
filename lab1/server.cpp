// server.cpp
//
// MinGW:
//   g++ -std=c++17 server.cpp -lws2_32 -o server.exe
//
// Simple multi-client chat server using Winsock2 + std::thread.
// Protocol: 4-byte big-endian length + 1-byte type + payload (UTF-8)

#include "chatroom.h"

std::vector<ClientInfo> clients;            // 已连接客户端列表
std::atomic<int> room_count(0);             // 当前房间人数
std::mutex clients_mtx;                     // 保护 clients 列表的互斥锁
std::atomic<bool> server_running(true);     // 服务器运行标志
SOCKET listen_sock = INVALID_SOCKET;        // 监听 socket

// 广播消息给所有客户端，except 参数指定排除的 socket（一般是发送者自己）
void broadcast_except(SOCKET except, uint8_t type, const std::string& payload){
    std::lock_guard<std::mutex> lk(clients_mtx);
    for (auto it = clients.begin(); it != clients.end(); ){
        SOCKET s = it->sock;
        if (s == except) { ++it; continue; }
        if (!send_frame(s, type, payload)) {
            // 发送失败则移除该客户端
            closesocket(s);
            it = clients.erase(it);
        } else ++it;
    }
}

// 遍历，从 clients 列表中移除指定 socket 的客户端
void remove_client(SOCKET s){
    std::lock_guard<std::mutex> lk(clients_mtx);
    for (auto it = clients.begin(); it != clients.end(); ++it){
        if (it->sock == s){
            clients.erase(it);
            break;
        }
    }
}

// 检查昵称是否已存在
bool is_nickname_taken(const std::string& nickname){
    std::lock_guard<std::mutex> lk(clients_mtx);
    for (const auto& c : clients){
        if (c.nickname == nickname){
            return true;
        }
    }
    return false;
}

// 每个客户端连接对应的线程函数
void client_thread_func(ClientInfo ci){
    // 获取客户端 socket 和昵称
    SOCKET s = ci.sock;
    std::string nickname = ci.nickname;
    // 广播用户加入消息（加入本人除外）
    std::string joinmsg = '[' + nickname + " joined]";
    broadcast_except(s, SERVER_NOTICE, joinmsg);

    // 主循环：只要服务器还在运行，一直监听接收并处理该客户端发送的消息
    while (server_running){
        // 读取长度信息（大端->小端转换，type+payload 总长度）
        uint32_t len_be;
        int rec = recv(s, (char*)&len_be, 4, MSG_WAITALL);  // 调用基本 socket 接收函数（流方式）
        if (rec <= 0) break;
        uint32_t len = ntohl(len_be);
        if (len < 1) break; // invalid

        // 读取类型信息
        uint8_t type;
        rec = recv(s, (char*)&type, 1, MSG_WAITALL);
        if (rec <= 0) break;

        // 读取 payload 内容
        int payload_len = (int)len - 1;
        std::string payload;
        if (payload_len > 0) {
            payload.resize(payload_len);
            rec = recv(s, &payload[0], payload_len, MSG_WAITALL);
            if (rec <= 0) break;
        }

        if (type == CLIENT_MSG) {
            // 广播用户发送的消息（除本人外）
            std::string out = nickname + ": " + payload;
            broadcast_except(s, SERVER_BROADCAST, out);
        } else if (type == CLIENT_LOGOUT) {
            // 用户登出，跳出循环结束线程
            break;
        } else {
            // 其他，暂时忽略
        }
    }

    // 清理工作
    closesocket(s);
    remove_client(s);
    room_count--;   // 减少房间人数
    std::string leave = '[' + nickname + " left]";
    broadcast_except(INVALID_SOCKET, SERVER_NOTICE, leave);
    std::cout << "\r" << "ADMIN:" << "\r";
    set_console_color(COLOR_YELLOW);
    std::cout << "User [" << nickname << "] disconnected\n";
    set_console_color(COLOR_DEFAULT);
    std::cout << "Current room users: " << room_count.load() << std::endl;
    set_console_color(COLOR_CYAN);
    std::cout << "ADMIN: ";
    set_console_color(COLOR_DEFAULT);
    std::cout.flush();
}

// 服务器端监听线程函数：接受新连接
void accept_thread_func(){
    // 主循环：只要服务器还在运行，一直监听接受新连接
    while (server_running){
        SOCKADDR_IN clientAddr;
        int addrlen = sizeof(clientAddr);
        SOCKET clientSock = accept(listen_sock, (SOCKADDR*)&clientAddr, &addrlen);  // 调用基本的 socket 接受连接函数
        // 服务器停止运行，跳出循环
        if (clientSock == INVALID_SOCKET) {
            if (!server_running) break;
            // 未停止运行而接受失败，报错
            std::cout << "\r" << "ADMIN:" << "\r";
            set_console_color(COLOR_RED);
            std::cerr << "[ERROR] accept failed\n";
            set_console_color(COLOR_DEFAULT);
            set_console_color(COLOR_CYAN);
            std::cout << "ADMIN: ";
            set_console_color(COLOR_DEFAULT);
            std::cout.flush();
            continue;
        }

        // 初次握手：尝试读取登录 LOGIN 帧
        uint32_t len_be;
        int rec = recv(clientSock, (char*)&len_be, 4, MSG_WAITALL);
        if (rec <= 0) { closesocket(clientSock); continue; }    // 无法读取有效 len 导致握手失败，回到循环等待下一个连接
        uint32_t len = ntohl(len_be);
        if (len < 1) { closesocket(clientSock); continue; }     // 登录帧定义无效
        uint8_t type;
        rec = recv(clientSock, (char*)&type, 1, MSG_WAITALL);   
        if (rec <= 0) { closesocket(clientSock); continue; }    // 无法读取有效 type 导致握手失败，回到循环等待下一个连接
        int payload_len = (int)len - 1;
        std::string payload;
        if (payload_len > 0) {                                  // payload 长度大于 0（有效）则读取 payload 内容
            payload.resize(payload_len);
            rec = recv(clientSock, &payload[0], payload_len, MSG_WAITALL);
            if (rec <= 0) { closesocket(clientSock); continue; }// 无法读取有效 payload 导致握手失败，回到循环等待下一个连接
        }
        // 初次握手成功，成功读取 LOGIN 帧全部信息

        // 验证登录信息
        if (type != CLIENT_LOGIN || payload.empty()){
            // 无效 LOGIN 帧，依旧取消连接重回等待
            send_frame(clientSock, SERVER_NOTICE, "Login required");
            closesocket(clientSock);
            continue;
        }

        // 检查昵称是否重复
        if (is_nickname_taken(payload)){
            send_frame(clientSock, SERVER_LOGIN_REJECT, "Nickname already taken");
            closesocket(clientSock);
            std::cout << "\r" << "ADMIN:" << "\r";
            set_console_color(COLOR_RED);
            std::cout << "[ERROR] Login rejected: nickname '" << payload << "' already in use\n";
            set_console_color(COLOR_DEFAULT);
            set_console_color(COLOR_CYAN);
            std::cout << "ADMIN: ";
            set_console_color(COLOR_DEFAULT);
            std::cout.flush();
            continue;
        }

        // LOGIN 有效，添加客户端到服务器端 client 列表
        ClientInfo ci;
        ci.sock = clientSock;
        ci.nickname = payload;

        // 添加时使用互斥锁保护 clients 列表
        {
            std::lock_guard<std::mutex> lk(clients_mtx);
            clients.push_back(ci);
            room_count++;  // 增加房间人数
        }
        
        // 为该客户端启动一个新的服务端<->客户端通信线程，处理后续通信
        std::thread t(client_thread_func, ci);
        t.detach();                             // 分离线程，交由系统自行回收
        
        std::cout << "\r" << "ADMIN:" << "\r";
        set_console_color(COLOR_GREEN);
        std::cout << "User [" << payload << "] connected\n";
        set_console_color(COLOR_DEFAULT);
        std::cout << "Current room users: " << room_count.load() << std::endl;
        set_console_color(COLOR_CYAN);
        std::cout << "ADMIN: ";
        set_console_color(COLOR_DEFAULT);
        std::cout.flush();
    }
}

int main(){
    // 创建全局互斥量，确保只有一个服务器实例运行
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "Global\\ChatServerMutex_12345");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        set_console_color(COLOR_RED);
        std::cerr << "[ERROR] Another server instance is already running on port 12345.\n";
        set_console_color(COLOR_DEFAULT);
        std::cerr << "Press any key to exit...\n";
        _getch();  // 等待按键
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

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
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    // 创建服务器端监听（TCP 流方式）socket
    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        std::cerr << "socket failed\n";
        WSACleanup();
        return 1;
    }

    // 绑定地址和端口，开始监听
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = INADDR_ANY;
    srv.sin_port = htons(12345);                // 默认端口

    int opt = 1;                                // 设置地址复用选项，避免重启服务器时地址被占用
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    if (bind(listen_sock, (sockaddr*)&srv, sizeof(srv)) == SOCKET_ERROR) {  // 调用基本 socket 绑定函数，绑定地址和端口
        std::cerr << "bind failed\n";           // 绑定失败处理
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {                   // 调用基本 socket 监听函数，开始监听连接请求
        std::cerr << "listen failed\n";         // 监听失败处理
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    // console 输出服务器启动信息
    std::cout << "Chat server started on port 12345\n";
    std::cout << "Type '/exit' to shutdown server\n";

    // 启动接受连接线程 accept_thread_func
    // 新建的 accept_th 是负责接受新连接的线程类实例
    std::thread accept_th(accept_thread_func);

    // console 主循环
    // 监听 console 输入，等待 /exit 命令以关闭服务器
    std::string line;
    while (server_running) {
        set_console_color(COLOR_CYAN);
        std::cout << "ADMIN: ";
        set_console_color(COLOR_DEFAULT);
        std::cout.flush();
        
        if (!read_console_line(line)) {
            break;
        }
        
        // 停止运行
        if (line == "/exit") {
            server_running = false;
            break;
        }
        // 服务器端以管理员身份广播消息
        std::lock_guard<std::mutex> lk(clients_mtx);    // 依旧锁机制保护 clients 列表
        for (auto &c : clients) {
            send_frame(c.sock, SERVER_NOTICE, std::string("★ADMIN★ ") + line);
        }
    }
    // 停止运行后退出循环

    // 关闭服务器，清理资源
    std::cout << "[TERMINATED] Shutting down...\n";
    closesocket(listen_sock);

    {
        // 通知所有客户端服务器关闭，并关闭它们的 socket
        std::lock_guard<std::mutex> lk(clients_mtx);
        for (auto &c : clients) {
            send_frame(c.sock, SERVER_NOTICE, "Server is shutting down");
            closesocket(c.sock);
        }
        // 清空客户端列表
        clients.clear();
    }

    // 等待接受连接的线程退出
    if (accept_th.joinable()) accept_th.join();

    WSACleanup();       // 清理 Winsock 资源
    std::cout << "[TERMINATED] Server stopped.\n";

    // 释放互斥量
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return 0;
}
