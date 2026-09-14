/// TASK-038 · ClientLink 实现（winsock2 客户端 + 4 字节大端长度前缀帧）。

#include "client_link.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cstdlib>
#include <cstring>

namespace mmo { namespace bot {

#ifdef _WIN32
using SockT = SOCKET;
using SsizeT = int;
static constexpr SockT kBad = INVALID_SOCKET;
#else
using SockT = int;
using SsizeT = ssize_t;
static constexpr SockT kBad = -1;
#endif

void ClientLink::PutBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}

std::uint32_t ClientLink::GetBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           (static_cast<std::uint32_t>(p[3]));
}

ClientLink::ClientLink() = default;

ClientLink::~ClientLink() {
    Close();
#ifdef _WIN32
    if (ws_) { WSACleanup(); }
#endif
}

core::Result<void> ClientLink::Connect(std::string_view addr, std::uint32_t connect_timeout_ms) {
    if (IsOpen()) return core::Result<void>::Ok();

    // 解析 host:port
    const auto colon = addr.rfind(':');
    if (colon == std::string_view::npos) {
        return core::Result<void>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "gateway addr 缺少端口", core::domain::kNet));
    }
    const std::string host(addr.substr(0, colon));
    const std::string port_str(addr.substr(colon + 1));
    const int port = std::atoi(port_str.c_str());
    if (port <= 0 || port > 65535) {
        return core::Result<void>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "端口非法", core::domain::kNet));
    }

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return core::Result<void>::Fail(
            core::Error(core::ErrorCode::INTERNAL_ERROR, "WSAStartup 失败", core::domain::kNet));
    }
    ws_ = true;
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || res == nullptr) {
        return core::Result<void>::Fail(
            core::Error(core::ErrorCode::NOT_FOUND, "无法解析网关地址", core::domain::kNet));
    }

    SockT s = kBad;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kBad) continue;
#ifdef _WIN32
        // 非阻塞 connect 实现超时。
        // FIONBIO=0x8004667E 是无符号常量，而 ioctlsocket 形参是 long：
        // 直接传会触发 -Wsign-conversion（值被"变成"负数，虽在 Windows 上行为正确但告警刺眼），
        // 这里显式转换一次，既消警也表明是有意为之。
        constexpr long kFionbio = static_cast<long>(FIONBIO);
        u_long mode = 1;
        ioctlsocket(s, kFionbio, &mode);
#else
        fcntl(s, F_SETFL, O_NONBLOCK);
#endif
        const int cr = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool connected = false;
        if (cr == 0) {
            connected = true;
        } else {
#ifdef _WIN32
            const int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
#else
            const int e = errno;
            if (e == EINPROGRESS) {
#endif
                fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
                timeval tv{};
                tv.tv_sec = static_cast<long>(connect_timeout_ms / 1000);
                tv.tv_usec = static_cast<long>((connect_timeout_ms % 1000) * 1000);
                const int sr = ::select(static_cast<int>(s) + 1, nullptr, &wf, nullptr, &tv);
                if (sr > 0) connected = true;
            }
        }
        if (connected) {
#ifdef _WIN32
            mode = 0; ioctlsocket(s, kFionbio, &mode);  // 复用外层 mode，避免遮蔽（-Wshadow）
#else
            fcntl(s, F_SETFL, 0);
#endif
            sock_ = static_cast<std::uintptr_t>(s);
            freeaddrinfo(res);
            return core::Result<void>::Ok();
        }
        Close();
    }
    if (res) freeaddrinfo(res);
    return core::Result<void>::Fail(
        core::Error(core::ErrorCode::TIMEOUT, "连接网关超时", core::domain::kNet));
}

core::Result<void> ClientLink::Send(std::string_view payload) {
    if (!IsOpen()) {
        return core::Result<void>::Fail(
            core::Error(core::ErrorCode::UNAUTHORIZED, "连接未建立", core::domain::kNet));
    }
    if (payload.size() > (1u << 20)) {
        return core::Result<void>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "payload 超过 1MiB", core::domain::kNet));
    }
    const std::uint32_t n = static_cast<std::uint32_t>(payload.size());
    std::uint8_t hdr[4];
    PutBe32(hdr, n);
    SockT s = static_cast<SockT>(sock_);
    auto send_all = [&](const char* buf, std::size_t len) -> bool {
        std::size_t off = 0;
        while (off < len) {
#ifdef _WIN32
            const SsizeT w = ::send(s, buf + off, static_cast<int>(len - off), 0);
#else
            const SsizeT w = ::send(s, buf + off, len - off, MSG_NOSIGNAL);
#endif
            if (w <= 0) return false;
            off += static_cast<std::size_t>(w);
        }
        return true;
    };
    if (!send_all(reinterpret_cast<const char*>(hdr), 4)) {
        return core::Result<void>::Fail(
            core::Error(core::ErrorCode::INTERNAL_ERROR, "发送长度前缀失败", core::domain::kNet));
    }
    if (!send_all(payload.data(), payload.size())) {
        return core::Result<void>::Fail(
            core::Error(core::ErrorCode::INTERNAL_ERROR, "发送 payload 失败", core::domain::kNet));
    }
    return core::Result<void>::Ok();
}

core::Result<std::vector<std::uint8_t>> ClientLink::Recv(std::uint32_t recv_timeout_ms) {
    std::vector<std::uint8_t> out;
    if (!IsOpen()) {
        return core::Result<std::vector<std::uint8_t>>::Fail(
            core::Error(core::ErrorCode::UNAUTHORIZED, "连接未建立", core::domain::kNet));
    }
    SockT s = static_cast<SockT>(sock_);

    // 超时控制
    if (recv_timeout_ms > 0) {
        fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
        timeval tv{};
        tv.tv_sec = static_cast<long>(recv_timeout_ms / 1000);
        tv.tv_usec = static_cast<long>((recv_timeout_ms % 1000) * 1000);
        const int sr = ::select(static_cast<int>(s) + 1, &rf, nullptr, nullptr, &tv);
        if (sr == 0) {
            return core::Result<std::vector<std::uint8_t>>::Fail(
                core::Error(core::ErrorCode::TIMEOUT, "接收超时", core::domain::kNet));
        } else if (sr < 0) {
            return core::Result<std::vector<std::uint8_t>>::Fail(
                core::Error(core::ErrorCode::INTERNAL_ERROR, "select 失败", core::domain::kNet));
        }
    }

    auto recv_full = [&](char* buf, std::size_t len) -> bool {
        std::size_t off = 0;
        while (off < len) {
#ifdef _WIN32
            const SsizeT r = ::recv(s, buf + off, static_cast<int>(len - off), 0);
#else
            const SsizeT r = ::recv(s, buf + off, len - off, 0);
#endif
            if (r <= 0) return false;
            off += static_cast<std::size_t>(r);
        }
        return true;
    };

    std::uint8_t hdr[4];
    if (!recv_full(reinterpret_cast<char*>(hdr), 4)) {
        return core::Result<std::vector<std::uint8_t>>::Fail(
            core::Error(core::ErrorCode::INTERNAL_ERROR, "读取长度前缀失败", core::domain::kNet));
    }
    const std::uint32_t n = GetBe32(hdr);
    if (n == 0 || n > (1u << 20)) {
        return core::Result<std::vector<std::uint8_t>>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "非法帧长度", core::domain::kNet));
    }
    out.resize(n);
    if (!recv_full(reinterpret_cast<char*>(out.data()), n)) {
        return core::Result<std::vector<std::uint8_t>>::Fail(
            core::Error(core::ErrorCode::INTERNAL_ERROR, "读取 payload 失败", core::domain::kNet));
    }
    return core::Result<std::vector<std::uint8_t>>::Ok(std::move(out));
}

void ClientLink::Close() noexcept {
    if (sock_ != kInvalidSock) {
        SockT s = static_cast<SockT>(sock_);
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
        sock_ = kInvalidSock;
    }
}

}}  // namespace mmo::bot
