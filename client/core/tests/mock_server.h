#pragma once

/// TASK-034 · 测试用 MockServer（进程内 winsock 服务端，复用真实协议编解码）。
///
/// 用途：让 client_test 在无真实服务端时跑通真实 TCP + 真实 Envelope 编解码：
///   - 对每条收到的 Command 回一个合法 Response（沿用同一 request_id / version）；
///   - 收首条 Command 后，自动推送预置的 Event 列表（负载为任意字节，由测试侧解释）；
///   - DropClient() 关闭当前连接（模拟网络中断），但继续监听，便于验证断线重连。

#include "mmo/core/error/result.h"
#include "mmo/protocol/codec/flatbuf_codec.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

namespace mmo { namespace client { namespace test {

#ifdef _WIN32
using SockT = SOCKET;
using SsizeT = int;
static constexpr SockT kBad = INVALID_SOCKET;
#else
using SockT = int;
using SsizeT = ssize_t;
static constexpr SockT kBad = -1;
#endif

inline void PutBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}
inline std::uint32_t GetBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           (static_cast<std::uint32_t>(p[3]));
}

class MockServer {
public:
    MockServer() = default;
    ~MockServer() { Stop(); }

    MockServer(const MockServer&) = delete;
    MockServer& operator=(const MockServer&) = delete;

    core::Result<std::uint16_t> Start(std::uint16_t port = 0) {
        if (running_.exchange(true)) return core::Result<std::uint16_t>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "已在运行", core::domain::kNet));
#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return core::Result<std::uint16_t>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "WSAStartup 失败", core::domain::kNet));
        ws_ = true;
#endif
        SockT s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == kBad) return core::Result<std::uint16_t>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "socket 失败", core::domain::kNet));
        int opt = 1;
#ifdef _WIN32
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return core::Result<std::uint16_t>::Fail(core::Error(
                core::ErrorCode::INTERNAL_ERROR, "bind 失败", core::domain::kNet));
        }
        if (::listen(s, 8) != 0) {
            return core::Result<std::uint16_t>::Fail(core::Error(
                core::ErrorCode::INTERNAL_ERROR, "listen 失败", core::domain::kNet));
        }
        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        ::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);
        listen_sock_ = static_cast<std::uintptr_t>(s);
        accept_thread_ = std::thread(&MockServer::AcceptLoop, this);
        return core::Result<std::uint16_t>::Ok(port_);
    }

    void Stop() noexcept {
        if (!running_.exchange(false)) return;
        if (listen_sock_ != kInvalidSockUint) {
            SockT s = static_cast<SockT>(listen_sock_);
#ifdef _WIN32
            ::closesocket(s);
#else
            ::close(s);
#endif
            listen_sock_ = kInvalidSockUint;
        }
        DropClient();
        if (accept_thread_.joinable()) accept_thread_.join();
#ifdef _WIN32
        if (ws_) { WSACleanup(); ws_ = false; }
#endif
    }

    /// 关闭当前连接（模拟网络中断），但继续监听。
    void DropClient() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (conn_sock_ != kInvalidSockUint) {
            SockT s = static_cast<SockT>(conn_sock_);
#ifdef _WIN32
            ::closesocket(s);
#else
            ::close(s);
#endif
            conn_sock_ = kInvalidSockUint;
        }
    }

    /// 预置自动推送的 Event 负载（收首条 Command 后依次发送）。
    void SetAutoEvents(std::vector<std::vector<std::uint8_t>> events) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto_events_ = std::move(events);
    }

    std::uint16_t Port() const noexcept { return port_; }
    std::string Addr() const { return "127.0.0.1:" + std::to_string(port_); }

private:
    static constexpr std::uintptr_t kInvalidSockUint = ~static_cast<std::uintptr_t>(0);

    void AcceptLoop() {
        while (running_.load()) {
            SockT s = static_cast<SockT>(listen_sock_);
            if (s == kBad) break;
            fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
            timeval tv{}; tv.tv_usec = 200'000;
            const int sr = ::select(static_cast<int>(s) + 1, &rf, nullptr, nullptr, &tv);
            if (sr <= 0) continue;
            SockT c = ::accept(s, nullptr, nullptr);
            if (c == kBad) continue;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                conn_sock_ = static_cast<std::uintptr_t>(c);
            }
            Serve(c);
        }
    }

    void Serve(SockT c) {
        protocol::FlatbufCodec codec;
        bool first = true;
        while (running_.load()) {
            auto frame = RecvAll(c);
            if (!frame.HasValue()) break;
            auto dec = codec.Decode(std::string_view(
                reinterpret_cast<const char*>(frame.Value().data()), frame.Value().size()));
            if (!dec.HasValue()) break;
            const auto& v = dec.Value().view();
            // 构造 Response（沿用 request_id / version）
            protocol::EnvelopeView r;
            r.message_id = v.message_id;
            r.request_id = v.request_id;
            r.message_type = protocol::EnvelopeMessageType::Response;
            r.version = v.version;
            r.source = "mock";
            r.payload = v.payload;
            auto enc = codec.Encode(r);
            if (!enc.HasValue()) break;
            if (!SendAll(c, enc.Value()).HasValue()) break;

            if (first) {
                first = false;
                std::vector<std::vector<std::uint8_t>> evs;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    evs = auto_events_;
                }
                for (auto& payload : evs) {
                    protocol::EnvelopeView e;
                    e.message_id = ++ev_id_;
                    e.request_id = 0;
                    e.message_type = protocol::EnvelopeMessageType::Event;
                    e.version = v.version;
                    e.source = "mock";
                    e.payload = std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size());
                    auto eenc = codec.Encode(e);
                    if (eenc.HasValue()) (void)SendAll(c, eenc.Value());
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (conn_sock_ == static_cast<std::uintptr_t>(c)) conn_sock_ = kInvalidSockUint;
        }
    }

    core::Result<std::vector<std::uint8_t>> RecvAll(SockT s) {
        std::uint8_t hdr[4];
        auto rd = [&](char* b, std::size_t n) -> bool {
            std::size_t off = 0;
            while (off < n) {
#ifdef _WIN32
                SsizeT x = ::recv(s, b + off, static_cast<int>(n - off), 0);
#else
                SsizeT x = ::recv(s, b + off, n - off, 0);
#endif
                if (x <= 0) return false;
                off += static_cast<std::size_t>(x);
            }
            return true;
        };
        if (!rd(reinterpret_cast<char*>(hdr), 4)) return core::Result<std::vector<std::uint8_t>>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "读取长度前缀失败", core::domain::kNet));
        const std::uint32_t n = GetBe32(hdr);
        if (n == 0 || n > (1u << 20)) return core::Result<std::vector<std::uint8_t>>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "非法帧长度", core::domain::kNet));
        std::vector<std::uint8_t> out(n);
        if (!rd(reinterpret_cast<char*>(out.data()), n)) return core::Result<std::vector<std::uint8_t>>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "读取 payload 失败", core::domain::kNet));
        return core::Result<std::vector<std::uint8_t>>::Ok(std::move(out));
    }

    core::Result<void> SendAll(SockT s, const std::vector<std::uint8_t>& bytes) {
        std::uint8_t hdr[4];
        PutBe32(hdr, static_cast<std::uint32_t>(bytes.size()));
        auto wr = [&](const char* b, std::size_t n) -> bool {
            std::size_t off = 0;
            while (off < n) {
#ifdef _WIN32
                SsizeT x = ::send(s, b + off, static_cast<int>(n - off), 0);
#else
                SsizeT x = ::send(s, b + off, n - off, MSG_NOSIGNAL);
#endif
                if (x <= 0) return false;
                off += static_cast<std::size_t>(x);
            }
            return true;
        };
        if (!wr(reinterpret_cast<const char*>(hdr), 4)) return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "发送长度前缀失败", core::domain::kNet));
        if (!wr(reinterpret_cast<const char*>(bytes.data()), bytes.size())) return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "发送 payload 失败", core::domain::kNet));
        return core::Result<void>::Ok();
    }

    std::atomic<bool> running_{false};
    std::uint16_t port_{0};
    std::uintptr_t listen_sock_ = kInvalidSockUint;
    std::uintptr_t conn_sock_ = kInvalidSockUint;
    std::uint64_t ev_id_{1};
    bool ws_ = false;
    std::thread accept_thread_;
    std::mutex mtx_;
    std::vector<std::vector<std::uint8_t>> auto_events_;
};

}}}  // namespace mmo::client::test
