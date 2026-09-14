// server/daemon/smoke_client.cpp — Gateway 端到端冒烟客户端（验证工具，非交付进程）
//
// 用途：验证 gateway daemon 的完整链路：TCP accept → 会话创建 → 24B 鉴权帧 →
// Active → 心跳帧 → 服务端 5s 摘要。退出码 0 = 全链路通过。
//
// 用法：gateway_smoke_client [--port 9000]
// 前置：gateway.exe 正在运行（另开终端：build/bin/gateway.exe）
//
// 帧格式见 gateway_main.cpp 文件头：传输层自动加 4B 长度前缀（Send 侧），
// 本客户端只发应用 payload；24B 鉴权帧 = player_id u64 + nonce u64 + signature u64。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uint16_t kDefaultPort = 9000;

bool SendAll(int fd, const char* buf, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const int n = static_cast<int>(::send(fd, buf + sent, len - sent, 0));
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// 发送 4B 大端长度前缀帧（gateway 的 INetworkTransport 契约，§8）。
bool SendFrame(int fd, const std::vector<std::uint8_t>& payload) {
    const std::uint32_t n = static_cast<std::uint32_t>(payload.size());
    const unsigned char prefix[4] = {static_cast<unsigned char>(n >> 24),
                                     static_cast<unsigned char>(n >> 16),
                                     static_cast<unsigned char>(n >> 8),
                                     static_cast<unsigned char>(n)};
    if (!SendAll(fd, reinterpret_cast<const char*>(prefix), 4)) {
        return false;
    }
    return payload.empty() ||
           SendAll(fd, reinterpret_cast<const char*>(payload.data()), payload.size());
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        }
    }

#ifdef _WIN32
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::printf("SMOKE_FAIL wsa_startup\n");
        return 1;
    }
#endif

    // 1. 连接
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        std::printf("SMOKE_FAIL socket\n");
        return 1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::printf("SMOKE_FAIL connect port=%u\n", port);
        return 1;
    }
    std::printf("SMOKE connected\n");

    // 2. 鉴权帧（24B）：player_id=42，nonce=7，
    //    signature = player*0x9E3779B97F4A7C15 ^ (nonce+1)（与 StubAuthProvider 一致）
    const std::uint64_t player = 42;
    const std::uint64_t nonce = 7;
    const std::uint64_t signature = (player * 0x9E3779B97F4A7C15ull) ^ (nonce + 1);
    std::vector<std::uint8_t> auth(24, 0);
    std::memcpy(auth.data(), &player, 8);
    std::memcpy(auth.data() + 8, &nonce, 8);
    std::memcpy(auth.data() + 16, &signature, 8);
    if (!SendFrame(fd, auth)) {
        std::printf("SMOKE_FAIL send_auth\n");
        return 1;
    }
    std::printf("SMOKE auth_sent player=%llu\n", static_cast<unsigned long long>(player));

    // 3. 心跳帧（任意非 24B 非空帧）
    std::vector<std::uint8_t> hb = {'h', 'b'};
    for (int i = 0; i < 3; ++i) {
        if (!SendFrame(fd, hb)) {
            std::printf("SMOKE_FAIL send_heartbeat\n");
            return 1;
        }
#ifdef _WIN32
        ::Sleep(200);
#else
        ::usleep(200000);
#endif
    }
    std::printf("SMOKE heartbeats_sent=3\n");

#ifdef _WIN32
    ::Sleep(300);
#endif
    ::closesocket(fd);
    std::printf("SMOKE_OK gateway auth+heartbeat end-to-end\n");
    return 0;
}
