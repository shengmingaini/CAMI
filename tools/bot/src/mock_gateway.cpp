/// TASK-038 · MockGateway 实现（winsock 服务端 + mmo::protocol Envelope 编解码）。

#include "mock_gateway.h"

#include "mmo/protocol/codec/envelope_view.h"
#include "mmo/protocol/codec/flatbuf_codec.h"
#include "mmo/protocol/codec/icodec.h"
#include "mmo/protocol/codec/owned_envelope.h"
#include "mmo/protocol/message_type.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cstring>

namespace mmo { namespace bot {

#ifdef _WIN32
using SockT = SOCKET;
static constexpr SockT kBad = INVALID_SOCKET;
#else
using SockT = int;
static constexpr SockT kBad = -1;
#endif

static void PutBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}
static std::uint32_t GetBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           (static_cast<std::uint32_t>(p[3]));
}
static bool SendFrame(SockT s, const std::uint8_t* data, std::size_t len) {
    std::uint8_t hdr[4]; PutBe32(hdr, static_cast<std::uint32_t>(len));
    auto sendall = [&](const char* b, std::size_t n) -> bool {
        std::size_t off = 0;
        while (off < n) {
#ifdef _WIN32
            const int w = ::send(s, b + off, static_cast<int>(n - off), 0);
#else
            const int w = ::send(s, b + off, n - off, MSG_NOSIGNAL);
#endif
            if (w <= 0) return false;
            off += static_cast<std::size_t>(w);
        }
        return true;
    };
    return sendall(reinterpret_cast<const char*>(hdr), 4) &&
           sendall(reinterpret_cast<const char*>(data), len);
}
// 返回 false 表示连接已关闭或出错。
static bool RecvFrame(SockT s, std::vector<std::uint8_t>& out) {
    std::uint8_t hdr[4];
    auto recvfull = [&](char* b, std::size_t n) -> bool {
        std::size_t off = 0;
        while (off < n) {
#ifdef _WIN32
            const int r = ::recv(s, b + off, static_cast<int>(n - off), 0);
#else
            const int r = ::recv(s, b + off, n - off, 0);
#endif
            if (r <= 0) return false;
            off += static_cast<std::size_t>(r);
        }
        return true;
    };
    if (!recvfull(reinterpret_cast<char*>(hdr), 4)) return false;
    const std::uint32_t n = GetBe32(hdr);
    if (n == 0 || n > (1u << 20)) return false;
    out.resize(n);
    return recvfull(reinterpret_cast<char*>(out.data()), n);
}

MockGateway::MockGateway() = default;
MockGateway::~MockGateway() { Stop(); }

core::Result<std::uint16_t> MockGateway::Start(std::uint16_t port) {
    if (running_.load()) return core::Result<std::uint16_t>::Ok(port_);

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return core::Result<std::uint16_t>::Fail(
            core::Error(core::ErrorCode::INTERNAL_ERROR, "WSAStartup 失败", core::domain::kNet));
#endif

    SockT s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBad) {
        return core::Result<std::uint16_t>::Fail(
            core::Error(core::ErrorCode::INTERNAL_ERROR, "socket 失败", core::domain::kNet));
    }
    int yes = 1;
#ifdef _WIN32
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
        return core::Result<std::uint16_t>::Fail(
            core::Error(core::ErrorCode::INTERNAL_ERROR, "bind 失败", core::domain::kNet));
    }
    if (::listen(s, 64) != 0) {
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
        return core::Result<std::uint16_t>::Fail(
            core::Error(core::ErrorCode::INTERNAL_ERROR, "listen 失败", core::domain::kNet));
    }
    // 取实际端口
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }
    listen_sock_ = static_cast<std::uintptr_t>(s);
    running_.store(true);
    accept_thread_ = std::thread(&MockGateway::AcceptLoop, this);
    return core::Result<std::uint16_t>::Ok(port_);
}

void MockGateway::Stop() noexcept {
    if (!running_.exchange(false)) return;
    if (listen_sock_ != 0) {
        SockT s = static_cast<SockT>(listen_sock_);
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
        listen_sock_ = 0;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    for (auto& t : conn_threads_) {
        if (t.joinable()) t.join();
    }
    conn_threads_.clear();
}

std::string MockGateway::Addr() const {
    return "127.0.0.1:" + std::to_string(port_);
}

void MockGateway::AcceptLoop() {
    const mmo::protocol::FlatbufCodec codec;
    while (running_.load()) {
        SockT c = ::accept(static_cast<SockT>(listen_sock_), nullptr, nullptr);
        if (c == kBad) {
            // listen 套接字被关闭 -> accept 失败，退出。
            break;
        }
        std::thread t(&MockGateway::ServeConnection, this, static_cast<std::uintptr_t>(c));
        {
            std::lock_guard<std::mutex> lk(mtx_);
            conn_threads_.push_back(std::move(t));
        }
    }
}

void MockGateway::ServeConnection(std::uintptr_t sock) {
    const mmo::protocol::FlatbufCodec codec;
    std::vector<std::uint8_t> buf;
    while (running_.load()) {
        if (!RecvFrame(static_cast<SockT>(sock), buf)) break;  // 客户端断开或出错
        auto dec = codec.Decode(
            std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
        if (!dec.HasValue()) break;
        const auto& v = dec.Value().view();
        // 构造合法 Response（沿用 request_id / version）
        mmo::protocol::EnvelopeView resp;
        resp.message_id = v.message_id;
        resp.message_type = mmo::protocol::EnvelopeMessageType::Response;
        resp.version = v.version ? v.version : 1;
        resp.source = "mock-gateway";
        resp.timestamp_ms = 0;
        resp.trace_id = v.trace_id;
        resp.request_id = v.request_id;
        resp.payload = "OK";
        auto enc = codec.Encode(resp);
        if (!enc.HasValue()) break;
        if (!SendFrame(static_cast<SockT>(sock), enc.Value().data(), enc.Value().size())) break;
    }
#ifdef _WIN32
    ::closesocket(static_cast<SockT>(sock));
#else
    ::close(static_cast<SockT>(sock));
#endif
}

}}  // namespace mmo::bot
