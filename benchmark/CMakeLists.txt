# CAMI Benchmark — TCP Echo Server & Client
# [PROTOTYPE] Tech stack verification

# Find Boost (asio is header-only; boost::system for error codes)
# CMP0167=NEW 已在根 CMakeLists 设置（继承至此子目录），这里显式 CONFIG 走 config 包。
find_package(Boost REQUIRED CONFIG COMPONENTS system)

# Find threads (portable: handles pthread on Linux, winpthreads on MinGW)
find_package(Threads REQUIRED)

# Windows (MinGW/MSVC) 必须显式链接 Winsock；Linux 上 socket 符号在 libc，无需此步。
# 不守卫会在 MinGW 链接期报 __imp_WSARecv / __imp_socket 等 undefined reference。
if(WIN32)
    set(CAMI_WINSOCK_LIBS ws2_32 mswsock)
else()
    set(CAMI_WINSOCK_LIBS "")
endif()

# === TCP Echo Server ===
add_executable(tcp_echo_server
    tcp_echo_server.cpp
)
target_link_libraries(tcp_echo_server
    PRIVATE
        Boost::system
        Threads::Threads
        ${CAMI_WINSOCK_LIBS}
)
target_compile_features(tcp_echo_server PRIVATE cxx_std_17)

# === TCP Echo Benchmark Client ===
add_executable(tcp_echo_benchmark
    tcp_echo_benchmark.cpp
)
target_link_libraries(tcp_echo_benchmark
    PRIVATE
        Boost::system
        Threads::Threads
        ${CAMI_WINSOCK_LIBS}
)
target_compile_features(tcp_echo_benchmark PRIVATE cxx_std_17)

# === Event Bus Throughput Benchmark (Day5) ===
# 验收: 总线吞吐 >= 1.00 Mmsg/s。仅用标准库线程，无需 Boost/Asio；
# WIN32 守卫的 ws2_32 在 Windows 上 harmless（本基准无 socket，但保持链接模式统一）。
add_executable(event_bus_bench
    event_bus_bench.cpp
)
target_link_libraries(event_bus_bench
    PRIVATE
        Threads::Threads
        ${CAMI_WINSOCK_LIBS}
)
target_compile_features(event_bus_bench PRIVATE cxx_std_17)

# === Gateway Codec Bandwidth Benchmark (Thu) ===
# 测量 codec 帧定界每字节处理代价（网关每消息 CPU 成本代理）。纯 std，仅链
# cami_gateway_codec（零外部依赖）；WIN32 守卫的 ws2_32 在本基准无 socket，harmless。
add_executable(gateway_bandwidth_bench
    gateway_bandwidth_bench.cpp
)
target_link_libraries(gateway_bandwidth_bench
    PRIVATE
        cami_gateway_codec
        Threads::Threads
        ${CAMI_WINSOCK_LIBS}
)
target_compile_features(gateway_bandwidth_bench PRIVATE cxx_std_17)

# === Gateway Stress Test (Fri) ===
# 真实 socket：server 用 ConnectionManager(SO_REUSEPORT 多 acceptor) 接受+持有，集成
# codec(帧定界) + heartbeat(超时踢线)；client 开 N 长连接周期发心跳帧保活。
# 链 cami_gateway_connection/codec/heartbeat + Boost::system + Winsock(Windows)。
add_executable(gateway_stress_server
    stress/gateway_stress_server.cpp
)
target_link_libraries(gateway_stress_server
    PRIVATE
        cami_gateway_connection
        cami_gateway_codec
        cami_gateway_heartbeat
        Boost::system
        Threads::Threads
        ${CAMI_WINSOCK_LIBS}
)
target_compile_features(gateway_stress_server PRIVATE cxx_std_17)

add_executable(gateway_stress_client
    stress/gateway_stress_client.cpp
)
target_link_libraries(gateway_stress_client
    PRIVATE
        cami_gateway_codec
        Boost::system
        Threads::Threads
        ${CAMI_WINSOCK_LIBS}
)
target_compile_features(gateway_stress_client PRIVATE cxx_std_17)
