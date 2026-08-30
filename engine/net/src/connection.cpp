// connection.cpp — TASK-008 §15.2 连接对象实现
//
// 线程边界：
//   - Send：业务线程调用，发送缓冲加锁（与 IO 线程 DrainSend 互斥）；
//   - DrainSend / RecvBuffer：IO 线程独占；
//   - Close：任意线程，只做状态标记（原子），真正的 socket 关闭在 IO 线程 Poll 循环完成。
//
// 优雅关闭（§15.2 / §21）：Close(Graceful) 只置 Closing，
//   IO 线程在 DrainSend 发现缓冲排空后调 ShutdownSend（FIN），再于回收阶段关闭 fd。

#include "mmo/net/connection.h"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mmo::net {

namespace {

/// 平台无关的「发送缓冲已满 / 暂不可写」判定。
bool IsWouldBlock() noexcept {
#if defined(_WIN32)
    const int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/// 平台无关的单次 send。返回 >0 已发送字节；返回 0 表示连接关闭；返回 -1 表示错误。
int SendOnce(SocketHandle fd, const std::uint8_t* data, std::size_t len) noexcept {
#if defined(_WIN32)
    const int n = ::send(static_cast<SOCKET>(fd),
                         reinterpret_cast<const char*>(data),
                         static_cast<int>(len), 0);
    return n == SOCKET_ERROR ? -1 : n;
#else
    return static_cast<int>(
        ::send(static_cast<int>(fd), data, len, MSG_NOSIGNAL));
#endif
}

void CloseSocket(SocketHandle fd) noexcept {
    if (fd == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(static_cast<int>(fd));
#endif
}

void ShutdownSend(SocketHandle fd) noexcept {
    if (fd == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::shutdown(static_cast<SOCKET>(fd), SD_SEND);
#else
    ::shutdown(static_cast<int>(fd), SHUT_WR);
#endif
}

}  // namespace

TcpConnection::TcpConnection(ConnectionId id, SocketHandle fd,
                             std::string remote_addr,
                             std::size_t recv_cap, std::size_t send_cap)
    : id_(id),
      fd_(fd),
      remote_addr_(std::move(remote_addr)),
      recv_buf_(recv_cap),
      send_buf_(send_cap) {}

TcpConnection::~TcpConnection() {
    CloseSocket(fd_);
}

core::Result<void> TcpConnection::Send(std::span<const std::uint8_t> data) {
    if (data.empty()) {
        return core::Result<void>::Ok();
    }
    if (data.size() > kMaxPayloadBytes) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "payload exceeds max",
            core::domain::kNet));
    }
    std::lock_guard<std::mutex> lk(send_mu_);
    if (state_.load(std::memory_order_acquire) != ConnState::Open) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "connection not open",
            core::domain::kNet));
    }
    // 帧 = 4B 大端长度前缀 + payload（与 Recv 侧剥前缀对称，§8 Packet）
    const std::size_t frame_len = kLengthPrefixBytes + data.size();
    if (send_buf_.Free() < frame_len) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "send buffer full", core::domain::kNet));
    }
    const std::uint8_t hdr[kLengthPrefixBytes] = {
        static_cast<std::uint8_t>(data.size() >> 24),
        static_cast<std::uint8_t>(data.size() >> 16),
        static_cast<std::uint8_t>(data.size() >> 8),
        static_cast<std::uint8_t>(data.size()),
    };
    // Free() 已预检 >= frame_len，两步写入必然成功（锁内无并发插入）
    (void)send_buf_.Write(hdr);
    (void)send_buf_.Write(data);
    stats_.packets_out.fetch_add(1, std::memory_order_relaxed);
    return core::Result<void>::Ok();
}

core::Result<void> TcpConnection::Close(CloseReason reason) noexcept {
    ConnState expected = ConnState::Open;
    if (!state_.compare_exchange_strong(expected, ConnState::Closing,
                                        std::memory_order_acq_rel)) {
        // 已处于 Closing/Closed：幂等，不覆盖首个原因
        return core::Result<void>::Ok();
    }
    close_reason_ = reason;
    return core::Result<void>::Ok();
}

bool TcpConnection::DrainSend() noexcept {
    std::lock_guard<std::mutex> lk(send_mu_);
    while (send_buf_.Size() > 0) {
        const auto r0 = send_buf_.ReadableRegion0();
        if (!r0.empty()) {
            const int n0 = SendOnce(fd_, r0.data(), r0.size());
            if (n0 < 0) {
                if (!IsWouldBlock()) {
                    AddError();
                }
                return false;  // 内核缓冲满（WOULD_BLOCK）或错误，等下次 Poll
            }
            if (n0 == 0) {
                return false;  // 对端关闭
            }
            send_buf_.CommitRead(static_cast<std::size_t>(n0));
            AddBytesOut(static_cast<std::uint64_t>(n0));
            if (static_cast<std::size_t>(n0) < r0.size()) {
                return false;  // 只发了一部分，内核缓冲满
            }
        }
        const auto r1 = send_buf_.ReadableRegion1();
        if (!r1.empty()) {
            const int n1 = SendOnce(fd_, r1.data(), r1.size());
            if (n1 < 0) {
                if (!IsWouldBlock()) {
                    AddError();
                }
                return false;
            }
            if (n1 == 0) {
                return false;
            }
            send_buf_.CommitRead(static_cast<std::size_t>(n1));
            AddBytesOut(static_cast<std::uint64_t>(n1));
            if (static_cast<std::size_t>(n1) < r1.size()) {
                return false;
            }
        }
    }
    // 缓冲已排空；若处于优雅关闭，立即发 FIN
    if (state_.load(std::memory_order_acquire) == ConnState::Closing &&
        close_reason_ == CloseReason::Graceful) {
        ShutdownSend(fd_);
    }
    return true;
}

ConnectionStats TcpConnection::Stats() const noexcept {
    ConnectionStats s;
    s.bytes_in = stats_.bytes_in.load(std::memory_order_relaxed);
    s.bytes_out = stats_.bytes_out.load(std::memory_order_relaxed);
    s.packets_in = stats_.packets_in.load(std::memory_order_relaxed);
    s.packets_out = stats_.packets_out.load(std::memory_order_relaxed);
    s.error_count = stats_.error_count.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(send_mu_);
    s.send_queue_bytes = send_buf_.Size();
    return s;
}

}  // namespace mmo::net
