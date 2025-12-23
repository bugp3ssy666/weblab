# 基于UDP的可靠传输协议 (Windows x86)

## 项目简介

本项目在UDP数据报套接字基础上实现了面向连接的可靠数据传输协议，使用Windows Socket API (Winsock2)，支持：

- ✅ 连接管理（两次握手建立/关闭）
- ✅ 差错检测（反码求和校验）
- ✅ 选择确认重传（SACK）
- ✅ 流量控制（固定窗口）
- ✅ 拥塞控制（TCP Reno算法）

## 文件结构

```
.
├── protocol.h          # 协议头文件和数据结构定义
├── sender.cpp          # 发送端/客户端实现
├── receiver.cpp        # 接收端/服务端实现
├── Makefile            # Makefile编译脚本（MinGW）
|
├── sender.exe          # 发送端/客户端可执行程序
├── receiver.exe        # 接收端/服务端可执行程序
├── Router.exe          # 实验本身提供的路由器模拟程序
├── *.jpg / *.txt       # 测试文件
|
└── README.md           # 本文件
```

## 环境要求

- **操作系统**: Windows 7/8/10/11
- **编译器**: 以下任一
  - Visual Studio 2015 或更高版本
  - MinGW-w64 (GCC 4.8+)
  - Clang for Windows
- **依赖**: Winsock2 (系统自带)

## 编译方法

### 方法1: 使用 g++ / MinGW-w64 (推荐，最简单)

#### 快速编译（一键脚本）:

**直接运行批处理文件**:
```cmd
build.bat
```

#### 手动编译:

```cmd
# 编译发送端
g++ -std=c++11 -O2 -o sender.exe sender.cpp -lws2_32

# 编译接收端
g++ -std=c++11 -O2 -o receiver.exe receiver.cpp -lws2_32
```

#### 使用 Makefile:

```cmd
# 编译全部
mingw32-make

# 或使用make（如果已安装）
make

# 清理
mingw32-make clean
```

**安装 MinGW-w64**:
- 下载: https://www.mingw-w64.org/
- 或使用 MSYS2: https://www.msys2.org/
- 确保将 `bin` 目录添加到系统 PATH

### 方法2: 使用 Visual Studio

#### 使用 Developer Command Prompt:

1. 打开 "开始菜单" → "Visual Studio 2022" → "Developer Command Prompt for VS 2022"

2. 进入项目目录:
```cmd
cd C:\path\to\your\project
```

3. 编译:
```cmd
cl /EHsc /std:c++14 /O2 /Fe:sender.exe sender.cpp ws2_32.lib
cl /EHsc /std:c++14 /O2 /Fe:receiver.exe receiver.cpp ws2_32.lib
```

#### 使用 Visual Studio IDE:

1. 创建新的 "空项目" (Empty Project)
2. 分别为 sender 和 receiver 创建两个项目
3. 添加相应的 .cpp 文件和 protocol.h
4. 项目属性 → 链接器 → 输入 → 附加依赖项：添加 `ws2_32.lib`
5. 编译生成

### 方法3: 使用 CMake

1. 创建 `CMakeLists.txt` 文件:
```cmake
cmake_minimum_required(VERSION 3.10)
project(ReliableUDP)

set(CMAKE_CXX_STANDARD 11)

add_executable(sender sender.cpp)
target_link_libraries(sender ws2_32)

add_executable(receiver receiver.cpp)
target_link_libraries(receiver ws2_32)
```

2. 编译:
```cmd
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## 使用方法

### 本地直连

**步骤1：启动接收端**

运行 `receiver.exe`，在 cmd 界面配制好服务器的 IP 地址和端口。

**步骤2：启动发送端**

运行 `sender.exe`，在 cmd 界面输入客户端 IP 地址和端口。注意，接收端 IP 地址和端口与 `receiver.exe` 中填写的保持一致。

**步骤3：输入文件目录**

在 `sender.exe` 中输入文件的绝对或相对目录（需要包含完整的文件名），回车确认即可开始文件传输。

### Router模拟连接

**步骤1：启动模拟路由器**

运行 `Router.exe`，在图形化界面中分别配置路由器的 IP 地址和端口、接收端的 IP 地址和端口。设置初始的丢包率和延迟大小参数。

**步骤2：启动接收端**

运行 `receiver.exe`，在 cmd 界面输入服务器的 IP 地址和端口，注意，要与路由器中填写的接收端保持一致。

**步骤3：启动发送端**

运行 `sender.exe`，在 cmd 界面输入客户端 IP 地址和端口。注意，接收端 IP 地址和端口与路由器的 IP 地址和端口保持一致。

**步骤4：输入文件目录**

在 `sender.exe` 中输入文件的绝对或相对目录（需要包含完整的文件名），回车确认即可开始文件传输。在路由器中可以观察数据包传输情况，包括丢包提示。

**步骤5：更改路由器设置**

`Router.exe` 中可以随时修改模拟路由器的丢包率和延迟大小，修改后点击“修改”键即生效。

## 支持的文件类型

- 📄 文本文件：.txt, .log, .md, .cpp, .h
- 🖼️ 图片文件：.jpg, .png, .bmp, .gif, .ico
- 📊 文档文件：.pdf, .doc, .docx, .ppt, .xlsx
- 🎵 媒体文件：.mp3, .mp4, .avi, .mkv
- 📦 压缩文件：.zip, .rar, .7z
- 💾 可执行文件：.exe, .dll
......

## 输出示例

### 发送端/客户端输出

```
========== 连接阶段 ==========
正在建立连接...
[✓] 已锁定服务器: 127.0.0.1:2
[✓] 已发送第三次握手ACK
[✓] 连接建立成功！

请输入要传输的文件路径: ./helloworld.txt
正在等待文件名确认...
[✓] 收到文件名确认，开始传输数据

========== 数据传输阶段 ==========
文件大小: 1655808 字节

========== 传输统计 ==========
[✓] 传输完成！
──────────────────────────────
  传输时间:    74202 ms
  吞吐率:      0.17 Mbps
  总字节数:    1875098
  总包数:      1799
  重传次数:    179
──────────────────────────────

========== 连接关闭阶段 ==========
正在关闭连接...
FIN包超时，进行第1次重传
[✓] 连接已安全关闭！
按任意键退出...
```

### 接收端/服务端输出

```
══════════ 接收端配置 ══════════
请输入绑定IP地址: 127.0.0.3
请输入端口号: 3

════════ 接收端已启动 ════════
监听端口: 3
等待连接中...

========== 连接建立 ==========
[✓] 已锁定客户端: 127.0.0.1:2
[✓] 收到SYN，建立连接
[✓] 收到第三次握手ACK，连接正式建立！

========== 数据接收 ==========
[✓] 输出文件已创建: helloworld_output.txt
[✓] 已发送FILE_NAME确认
/
========== 连接关闭 ==========
[✓] 收到FIN，关闭连接
[✓] 连接已安全关闭！

════════ 接收完成 ════════
──────────────────────────────
  总接收字节:  1655808
  总接收包数:  1621
──────────────────────────────
按任意键退出...
```
