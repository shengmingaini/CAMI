# gateway/connection/CMakeLists.txt — 连接管理模块 (spec §4.1)
# [PROTOTYPE] io_context 线程池 + 连接生命周期状态机 + SO_REUSEPORT 监听池。
# 仅依赖 Boost.Asio（header-only）+ boost::system，可独立编译进 CI（CAMI_BUILD_MODULES=OFF）。

find_package(Boost REQUIRED CONFIG COMPONENTS system)
find_package(Threads REQUIRED)

# Windows (MinGW/MSVC) 必须显式链接 Winsock；Linux 上 socket 符号在 libc，无需此步。
# 不守卫会在 MinGW 链接期报 __imp_WSARecv / __imp_socket 等 undefined reference（见优化-v2/v3 与 benchmark）。
if(WIN32)
    set(CAMI_WINSOCK_LIBS ws2_32 mswsock)
else()
    set(CAMI_WINSOCK_LIBS "")
endif()

# 连接管理模块：独立 STATIC target，满足"模块独立编译"验收。
add_library(cami_gateway_connection STATIC
    io_context_pool.cpp
    connection_fsm.cpp
    connection.cpp
    connection_manager.cpp
    connection_selfcheck.cpp
)

# 仓库根，支持 "gateway/connection/xxx.h" 形式包含（编码规范 §3）。
target_include_directories(cami_gateway_connection PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(cami_gateway_connection PUBLIC
    cami_gateway_codec          # Connection 内嵌 FrameDecoder (2026-08-12 热路径优化)
    Boost::system
    Threads::Threads
    ${CAMI_WINSOCK_LIBS}
)

target_compile_features(cami_gateway_connection PUBLIC cxx_std_17)
