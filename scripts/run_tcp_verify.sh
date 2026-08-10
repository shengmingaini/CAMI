#!/bin/bash
# CAMI Day2 TCP 验证全流程（在 MSYS2 mingw64 环境中运行）
# 等待 MinGW 工具链 + Boost 安装完成 -> 编译 -> 启动 server -> 运行 benchmark -> 输出 QPS
#
# 注意：必须在 MSYS2 bash 环境运行（/c/msys64/usr/bin/bash.exe -lc），
#       因为 boost::asio 需要 MinGW 的 DLL 路径（Git bash 直接调用 g++.exe 会 DLL 加载失败）。
#       boost 1.69+ 的 boost::system 是 header-only，无需 -lboost_system。
set -e

echo "[$(date +%T)] [1/4] 等待 MinGW gcc + Boost 安装完成..."
while [ ! -x /mingw64/bin/g++.exe ] || [ ! -f /mingw64/include/boost/asio.hpp ]; do
    sleep 15
done
echo "[$(date +%T)] [1/4] 工具链就绪: $(g++ --version | head -1)"

echo "[$(date +%T)] [2/4] 编译 TCP echo server + benchmark (header-only boost::asio)..."
cd /f/AI/workbuddy/CAMI/benchmark
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 tcp_echo_server.cpp -o /tmp/tcp_echo_server -lpthread -lws2_32 -lwsock32
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 tcp_echo_benchmark.cpp -o /tmp/tcp_echo_benchmark -lpthread -lws2_32 -lwsock32
echo "[$(date +%T)] [2/4] 编译完成"

echo "[$(date +%T)] [3/4] 启动 server (port 9000) 并运行 benchmark (8连接/64B/100万消息)..."
/tmp/tcp_echo_server 9000 &
SPID=$!
sleep 2
/tmp/tcp_echo_benchmark 127.0.0.1 9000 8 64 1000000 8
RC=$?
kill $SPID 2>/dev/null
echo "[$(date +%T)] [3/4] benchmark 退出码: $RC"

echo "[$(date +%T)] [4/4] TCP 验证流程结束。"
