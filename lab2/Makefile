# Makefile for Windows (MinGW-w64 / g++)

CXX = g++
CXXFLAGS = -std=c++11 -Wall -O2
LDFLAGS = -lws2_32

# 目标文件
TARGETS = sender.exe receiver.exe

# 默认目标
all: $(TARGETS)

# 编译发送端
sender.exe: sender.cpp protocol.h
	$(CXX) $(CXXFLAGS) -o sender.exe sender.cpp $(LDFLAGS)

# 编译接收端
receiver.exe: receiver.cpp protocol.h
	$(CXX) $(CXXFLAGS) -o receiver.exe receiver.cpp $(LDFLAGS)

# 清理编译文件
clean:
	del /Q sender.exe receiver.exe 2>nul

# 运行测试（需要两个终端）
test-receiver:
	receiver.exe 8888 output.txt

test-sender:
	sender.exe 127.0.0.1 8888 test.txt

.PHONY: all clean test-receiver test-sender