// tcp_transport.cpp — TASK-008 §15.3-8 TCP 第一版实现
//
// 线程模型（§4 / §9）：
//   - Poll 由宿主线程（Network 角色）独占驱动：连接表 / poller / socket 状态只在 Poll 内读写；
//   - 业务线程只能经 IConnection::Send / Close（内部加锁）提交请求；
//   - Received 事件 data 指向 event_pool_（本轮 Poll 有效），下轮 Poll 复用缓冲。

#include "tcp_transport.h"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mmo::net {

namespace {

/// 平台无关错误码转 Error。Windows 用 WSAGetLastError，POSIX 用 errno。
core::Error SocketError(std::string_view what) noexcept {
#if defined(_WIN32)
    const int err = WSAGetLastError();
    // 独占绑定（SO_EXCLUSIVEADDRUSE）被抢占时，Windows 返回 WSAEACCES 而非 WSAEADDRINUSE
    const bool in_use = (err == WSAEADDRINUSE || err == WSAEACCES);
    const bool would_block = (err == WSAEWOULDBLOCK);
#else
    const int err = errno;
    const bool in_use = (err == EADDRINUSE);
    const bool would_block = (err == EAGAIN || err == EWOULDBLOCK);
#endif
    (void)err;
    if (in_use) {
        return core::Error(core::ErrorCode::INVALID_ARGUMENT, "address already in use",
                           core::domain::kNet);
    }
    if (would_block) {
        return core::Error(core::ErrorCode::BUSY, "would block", core::domain::kNet);
    }
    return core::Error(core::ErrorCode::INTERNAL_ERROR, what, core::domain::kNet);
}

bool WouldBlock() noexcept {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void SetNonBlocking(SocketHandle fd) noexcept {
#if defined(_WIN32)
    u_long mode = 1;
    ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode);
#else
    const int flags = ::fcntl(static_cast<int>(fd), F_GETFL, 0);
    ::fcntl(static_cast<int>(fd), F_SETFL, flags | O_NONBLOCK);
#endif
}

void SetTcpNoDelay(SocketHandle fd, bool enable) noexcept {
    const int one = enable ? 1 : 0;
#if defined(_WIN32)
    ::setsockopt(static_cast<SOCKET>(fd), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

void CloseFd(SocketHandle fd) noexcept {
    if (fd == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(static_cast<int>(fd));
#endif
}

int AcceptOnce(SocketHandle listen_fd, SocketHandle& out_fd) noexcept {
#if defined(_WIN32)
    const SOCKET c = ::accept(static_cast<SOCKET>(listen_fd), nullptr, nullptr);
    if (c == INVALID_SOCKET) {
        return -1;
    }
    out_fd = static_cast<SocketHandle>(c);
    return 0;
#else
    const int c = ::accept(static_cast<int>(listen_fd), nullptr, nullptr);
    if (c < 0) {
        return -1;
    }
    out_fd = static_cast<SocketHandle>(c);
    return 0;
#endif
}

int RecvOnce(SocketHandle fd, std::uint8_t* dst, std::size_t len) noexcept {
#if defined(_WIN32)
    const int n = ::recv(static_cast<SOCKET>(fd), reinterpret_cast<char*>(dst),
                         static_cast<int>(len), 0);
    return n == SOCKET_ERROR ? -1 : n;
#else
    return static_cast<int>(::recv(static_cast<int>(fd), dst, len, 0));
#endif
}

}  // namespace

TcpTransport::TcpTransport(TcpConfig cfg)
    : cfg_(cfg),
      slots_(cfg_.max_connections),
      id_alloc_(cfg_.max_connections) {
#if defined(_WIN32)
    // Windows 需要一次性 WSAStartup（进程级，幂等）
    static const bool wsa_ok = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)wsa_ok;
#endif
}

TcpTransport::~TcpTransport() {
    std::vector<TransportEvent> ignored;
    CloseAll(ignored);
}

core::Result<void> TcpTransport::Listen(std::string_view addr, std::uint16_t port) {
    if (listening_) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "already listening", core::domain::kNet));
    }
    const auto result = SetupListenSocket(addr, port);
    if (!result) {
        return result;
    }
    listening_ = true;
    return core::Result<void>::Ok();
}

core::Result<void> TcpTransport::SetupListenSocket(std::string_view addr,
                                                   std::uint16_t port) {
#if defined(_WIN32)
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return core::Result<void>::Fail(
            SocketError("socket() failed"));
    }
    listen_fd_ = static_cast<SocketHandle>(s);
#else
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return core::Result<void>::Fail(SocketError("socket() failed"));
    }
    listen_fd_ = static_cast<SocketHandle>(s);
#endif

    // 仅本机回环/显式地址：第一版支持 IPv4；IPv6 留待后续版本
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = ::htons(port);
    if (addr == "0.0.0.0" || addr.empty()) {
        sin.sin_addr.s_addr = ::htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, std::string(addr).c_str(), &sin.sin_addr) != 1) {
        CloseFd(listen_fd_);
        listen_fd_ = kInvalidSocket;
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "invalid ipv4 address", core::domain::kNet));
    }

#if defined(_WIN32)
    // Windows 端口独占：用 SO_EXCLUSIVEADDRUSE 而非 SO_REUSEADDR。
    // SO_REUSEADDR 在 Windows 上允许第二个 socket 重复绑定同一端口（与 POSIX 语义不同），
    // 会破坏"端口占用必须报错"的契约（§15.6 / 验收 port-in-use）。
    int opt = 1;
    if (::setsockopt(static_cast<SOCKET>(listen_fd_), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&opt), sizeof(opt)) == SOCKET_ERROR) {
        CloseFd(listen_fd_);
        listen_fd_ = kInvalidSocket;
        return core::Result<void>::Fail(SocketError("setsockopt(SO_EXCLUSIVEADDRUSE) failed"));
    }
    if (::bind(static_cast<SOCKET>(listen_fd_), reinterpret_cast<const sockaddr*>(&sin),
               sizeof(sin)) == SOCKET_ERROR) {
        // 先取错误（SocketError 依赖 WSAGetLastError），再关 fd——closesocket 会覆盖错误码
        const auto r = core::Result<void>::Fail(SocketError("bind() failed"));
        CloseFd(listen_fd_);
        listen_fd_ = kInvalidSocket;
        return r;
    }
    if (::listen(static_cast<SOCKET>(listen_fd_), cfg_.backlog) == SOCKET_ERROR) {
        const auto r = core::Result<void>::Fail(SocketError("listen() failed"));
        CloseFd(listen_fd_);
        listen_fd_ = kInvalidSocket;
        return r;
    }
#else
    int opt = 1;
    ::setsockopt(static_cast<int>(listen_fd_), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (::bind(static_cast<int>(listen_fd_), reinterpret_cast<const sockaddr*>(&sin),
               sizeof(sin)) < 0) {
        const auto r = core::Result<void>::Fail(SocketError("bind() failed"));
        CloseFd(listen_fd_);
        listen_fd_ = kInvalidSocket;
        return r;
    }
    if (::listen(static_cast<int>(listen_fd_), cfg_.backlog) < 0) {
        const auto r = core::Result<void>::Fail(SocketError("listen() failed"));
        CloseFd(listen_fd_);
        listen_fd_ = kInvalidSocket;
        return r;
    }
#endif
    SetNonBlocking(listen_fd_);
    poller_.Upsert(listen_fd_, true, false);
    return core::Result<void>::Ok();
}

core::Result<void> TcpTransport::Stop() noexcept {
    std::vector<TransportEvent> out;
    CloseAll(out);
    if (listen_fd_ != kInvalidSocket) {
        poller_.Remove(listen_fd_);
        CloseFd(listen_fd_);
        listen_fd_ = kInvalidSocket;
    }
    listening_ = false;
    return core::Result<void>::Ok();
}

core::Result<void> TcpTransport::Poll(core::DurationMs timeout,
                                      std::vector<TransportEvent>& out) {
    out.clear();
    event_pool_.clear();  // 上轮事件数据作废（契约：仅下一次 Poll 前有效）

    // 1. 每轮同步 poller 关注事件：有发送排队或正在关闭的连接加写关注
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto& host = slots_[i];
        if (!host.in_use) {
            continue;
        }
        const bool need_write =
            host.conn->HasPendingSend() || host.conn->CloseRequested();
        if (need_write != host.want_write) {
            poller_.Upsert(host.conn->Socket(), true, need_write);
            host.want_write = need_write;
        }
    }

    // 2. 阻塞等待事件
    const int n = poller_.Wait(timeout);
    if (n < 0) {
        error_count_.fetch_add(1, std::memory_order_relaxed);
        return core::Result<void>::Ok();  // poll 自身错误：跳过本轮（不崩溃）
    }
    if (n == 0) {
        CheckIdleTimeouts(out);
        return core::Result<void>::Ok();
    }

    // 3. 先收集就绪事件快照（WSAPoll 返回的 n 是就绪数，就绪项可能分布在任意下标，
    //    且后续处理可能 Remove 导致 poller 索引变化，因此必须先快照再处理）
    struct ReadyEntry {
        SocketHandle fd;
        PollEvent pe;
    };
    std::vector<ReadyEntry> ready;
    ready.reserve(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < poller_.Size(); ++i) {
        const PollEvent pe = poller_.EventAt(i);
        if (!pe.readable && !pe.writable && !pe.hangup && !pe.error) {
            continue;
        }
        ready.push_back({poller_.FdAt(i), pe});
    }

    // 4. 先处理监听 fd（accept），再处理连接事件
    bool listen_ready = false;
    for (const auto& r : ready) {
        if (r.fd == listen_fd_) {
            listen_ready = true;
            break;
        }
    }
    if (listen_ready) {
        ProcessAccept(out);
    }

    for (const auto& r : ready) {
        const SocketHandle fd = r.fd;
        if (fd == listen_fd_) {
            continue;
        }
        const auto it = fd_to_slot_.find(fd);
        if (it == fd_to_slot_.end()) {
            continue;  // 本轮刚被 accept 拒绝或已移除
        }
        auto& host = slots_[it->second];
        if (!host.in_use) {
            continue;
        }
        const PollEvent pe = r.pe;
        if (pe.error || pe.hangup) {
            RemoveConnection(host, CloseReason::Error, out);
            continue;
        }
        if (pe.readable) {
            ProcessRecv(host, out);
            if (!host.in_use) {
                continue;
            }
        }
        if (pe.writable) {
            ProcessSend(host, out);
        }
    }

    CheckIdleTimeouts(out);
    return core::Result<void>::Ok();
}

void TcpTransport::ProcessAccept(std::vector<TransportEvent>& out) {
    while (true) {
        SocketHandle cfd = kInvalidSocket;
        if (AcceptOnce(listen_fd_, cfd) < 0) {
            if (WouldBlock()) {
                break;  // 无更多 pending 连接
            }
            error_count_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        // 连接数上限：直接拒绝并计数（§15.7）
        if (conn_count_.load(std::memory_order_relaxed) >= cfg_.max_connections) {
            CloseFd(cfd);
            error_count_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        auto id_opt = id_alloc_.Allocate();
        if (!id_opt.has_value()) {
            CloseFd(cfd);
            error_count_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        const ConnectionId id = *id_opt;
        const std::size_t slot = static_cast<std::size_t>(id & 0xFFFFFFFFull);

        SetNonBlocking(cfd);
        SetTcpNoDelay(cfd, cfg_.tcp_nodelay);

        auto& host = slots_[slot];
        host.conn = std::make_unique<TcpConnection>(id, cfd, "peer", cfg_.recv_buf,
                                                    cfg_.send_buf);
        host.last_active_ns = core::MonotonicClock::Now();
        host.want_write = false;
        host.in_use = true;

        slot_of_[id] = slot;
        fd_to_slot_[cfd] = slot;
        poller_.Upsert(cfd, true, false);
        conn_count_.fetch_add(1, std::memory_order_relaxed);

        TransportEvent ev;
        ev.kind = TransportEvent::Kind::Connected;
        ev.conn_id = id;
        out.push_back(ev);
    }
}

void TcpTransport::ProcessRecv(ConnHost& host, std::vector<TransportEvent>& out) {
    auto& buf = host.conn->RecvBuffer();
    while (true) {
        auto r0 = buf.WritableRegion0();
        if (r0.empty()) {
            // 接收缓冲满：先尝试解出已完整的帧腾出空间（粘包场景）；
            // 若仍满则说明首帧长度超过缓冲容量（recv_buf < 帧大小），
            // 该帧永远无法凑齐 → 按协议违规断开，避免连接永久卡死（§16 大包需 cfg.recv_buf 足够大）。
            TryEmitPackets(host, out);
            if (!host.in_use) {
                return;
            }
            if (buf.Full()) {
                host.conn->AddError();
                error_count_.fetch_add(1, std::memory_order_relaxed);
                RemoveConnection(host, CloseReason::Error, out);
                return;
            }
            continue;
        }
        const int n = RecvOnce(host.conn->Socket(), r0.data(), r0.size());
        if (n < 0) {
            if (WouldBlock()) {
                break;
            }
            host.conn->AddError();
            error_count_.fetch_add(1, std::memory_order_relaxed);
            RemoveConnection(host, CloseReason::Error, out);
            return;
        }
        if (n == 0) {
            // 对端 FIN
            RemoveConnection(host, CloseReason::PeerClosed, out);
            return;
        }
        buf.CommitWrite(static_cast<std::size_t>(n));
        host.conn->AddBytesIn(static_cast<std::uint64_t>(n));
        bytes_in_.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
        host.last_active_ns = core::MonotonicClock::Now();
        if (static_cast<std::size_t>(n) < r0.size()) {
            break;  // 内核缓冲读空
        }
    }
    TryEmitPackets(host, out);
}

void TcpTransport::ProcessSend(ConnHost& host, std::vector<TransportEvent>& out) {
    const bool drained = host.conn->DrainSend();
    if (host.conn->CloseRequested()) {
        // 优雅关闭已排空（或非优雅直接关）→ 回收
        RemoveConnection(host, host.conn->Reason(), out);
        return;
    }
    if (drained) {
        // 发送缓冲排空：通知上层可继续推流
        TransportEvent ev;
        ev.kind = TransportEvent::Kind::SendDrained;
        ev.conn_id = host.conn->Id();
        out.push_back(ev);
    }
}

bool TcpTransport::PeekLength(const Buffer& buf, std::uint32_t& len) {
    if (buf.Size() < kLengthPrefixBytes) {
        return false;
    }
    std::uint8_t hdr[kLengthPrefixBytes];
    CopyOut(buf, hdr, kLengthPrefixBytes);
    len = (static_cast<std::uint32_t>(hdr[0]) << 24) |
          (static_cast<std::uint32_t>(hdr[1]) << 16) |
          (static_cast<std::uint32_t>(hdr[2]) << 8) |
          (static_cast<std::uint32_t>(hdr[3]));
    return true;
}

void TcpTransport::CopyOut(const Buffer& buf, std::uint8_t* dst, std::size_t n) {
    std::size_t copied = 0;
    const auto r0 = buf.ReadableRegion0();
    const std::size_t take0 = n < r0.size() ? n : r0.size();
    std::memcpy(dst, r0.data(), take0);
    copied += take0;
    if (copied < n) {
        const auto r1 = buf.ReadableRegion1();
        const std::size_t take1 = (n - copied) < r1.size() ? (n - copied) : r1.size();
        std::memcpy(dst + copied, r1.data(), take1);
    }
}

void TcpTransport::TryEmitPackets(ConnHost& host, std::vector<TransportEvent>& out) {
    auto& buf = host.conn->RecvBuffer();
    while (buf.Size() >= kLengthPrefixBytes) {
        std::uint32_t len = 0;
        if (!PeekLength(buf, len)) {
            break;
        }
        if (len > kMaxPayloadBytes) {
            // 超大包：拒绝并断开（§16 超大包拒绝）
            host.conn->AddError();
            error_count_.fetch_add(1, std::memory_order_relaxed);
            RemoveConnection(host, CloseReason::Error, out);
            return;
        }
        if (buf.Size() < kLengthPrefixBytes + len) {
            break;  // 半包：保留在缓冲，等后续数据（§15.5）
        }
        // 完整包：payload 拷贝到事件池（deque push_back 不失效已有元素引用）
        auto& payload = event_pool_.emplace_back();
        payload.resize(len);
        const auto first = buf.ReadableRegion0();
        std::size_t copied = 0;
        // 跳过 4 字节长度前缀
        if (first.size() >= kLengthPrefixBytes) {
            const std::size_t take =
                len < (first.size() - kLengthPrefixBytes) ? len : (first.size() - kLengthPrefixBytes);
            std::memcpy(payload.data(), first.data() + kLengthPrefixBytes, take);
            copied = take;
        } else {
            // 长度前缀跨段：从第二段补
            const auto r1 = buf.ReadableRegion1();
            const std::size_t skip_in_r1 = kLengthPrefixBytes - first.size();
            const std::size_t take =
                len < (r1.size() - skip_in_r1) ? len : (r1.size() - skip_in_r1);
            std::memcpy(payload.data(), r1.data() + skip_in_r1, take);
            copied = take;
        }
        if (copied < len) {
            // 回绕：剩余从缓冲头部（第二段）
            const auto tail = buf.ReadableRegion1();
            std::memcpy(payload.data() + copied, tail.data(), len - copied);
        }
        buf.Consume(kLengthPrefixBytes + len);
        host.conn->AddPacketsIn(1);
        packets_in_.fetch_add(1, std::memory_order_relaxed);
        host.last_active_ns = core::MonotonicClock::Now();

        TransportEvent ev;
        ev.kind = TransportEvent::Kind::Received;
        ev.conn_id = host.conn->Id();
        ev.data = payload;
        out.push_back(ev);
        ++frame_counter_;
    }
}

void TcpTransport::CheckIdleTimeouts(std::vector<TransportEvent>& out) {
    if (cfg_.keepalive_idle_s == 0) {
        return;
    }
    const core::SteadyNs now = core::MonotonicClock::Now();
    const core::SteadyNs idle_ns =
        static_cast<core::SteadyNs>(cfg_.keepalive_idle_s) * core::kSteadyNsPerSecond;
    for (auto& host : slots_) {
        if (!host.in_use) {
            continue;
        }
        if (now - host.last_active_ns > idle_ns) {
            RemoveConnection(host, CloseReason::Timeout, out);
        }
    }
}

void TcpTransport::RemoveConnection(ConnHost& host, CloseReason reason,
                                    std::vector<TransportEvent>& out) {
    const ConnectionId id = host.conn->Id();
    const SocketHandle fd = host.conn->Socket();

    TransportEvent ev;
    ev.kind = TransportEvent::Kind::Disconnected;
    ev.conn_id = id;
    switch (reason) {
        case CloseReason::Graceful:
            ev.error = core::Error(core::ErrorCode::OK, "graceful close",
                                   core::domain::kNet);
            break;
        case CloseReason::PeerClosed:
            ev.error = core::Error(core::ErrorCode::OK, "peer closed",
                                   core::domain::kNet);
            break;
        case CloseReason::Timeout:
            ev.error = core::Error(core::ErrorCode::TIMEOUT, "idle timeout",
                                   core::domain::kNet);
            break;
        default:
            ev.error = core::Error(core::ErrorCode::INTERNAL_ERROR, "transport error",
                                   core::domain::kNet);
            break;
    }
    out.push_back(ev);

    poller_.Remove(fd);
    fd_to_slot_.erase(fd);
    slot_of_.erase(id);
    id_alloc_.Release(id);
    host.conn->MarkClosed();
    host.conn.reset();  // 析构关闭 fd
    host.in_use = false;
    conn_count_.fetch_sub(1, std::memory_order_relaxed);
}

void TcpTransport::CloseAll(std::vector<TransportEvent>& out) {
    for (auto& host : slots_) {
        if (!host.in_use) {
            continue;
        }
        const ConnectionId id = host.conn->Id();
        const SocketHandle fd = host.conn->Socket();
        poller_.Remove(fd);
        fd_to_slot_.erase(fd);
        slot_of_.erase(id);
        id_alloc_.Release(id);
        host.conn->MarkClosed();
        host.conn.reset();
        host.in_use = false;
    }
    conn_count_.store(0, std::memory_order_relaxed);
    out.clear();
}

std::size_t TcpTransport::ConnectionCount() const noexcept {
    return conn_count_.load(std::memory_order_relaxed);
}

IConnection* TcpTransport::Get(ConnectionId id) noexcept {
    if (!id_alloc_.IsValid(id)) {
        return nullptr;
    }
    const std::size_t slot = static_cast<std::size_t>(id & 0xFFFFFFFFull);
    if (slot >= slots_.size() || !slots_[slot].in_use) {
        return nullptr;
    }
    auto& conn = slots_[slot].conn;
    if (!conn || conn->Id() != id) {
        return nullptr;  // generation 兜底（理论上 IsValid 已覆盖）
    }
    return conn.get();
}

TransportStats TcpTransport::Stats() const noexcept {
    TransportStats s;
    s.conn_count = conn_count_.load(std::memory_order_relaxed);
    s.bytes_in = bytes_in_.load(std::memory_order_relaxed);
    s.packets_in = packets_in_.load(std::memory_order_relaxed);
    s.error_count = error_count_.load(std::memory_order_relaxed);
    // 出方向统计在连接层累计（DrainSend/Send），这里聚合
    std::uint64_t depth = 0;
    for (const auto& host : slots_) {
        if (!host.in_use) {
            continue;
        }
        const ConnectionStats cs = host.conn->Stats();
        s.bytes_out += cs.bytes_out;
        s.packets_out += cs.packets_out;
        depth += cs.send_queue_bytes;
    }
    s.send_queue_depth = depth;
    return s;
}

std::unique_ptr<INetworkTransport> CreateTcpTransport(TcpConfig cfg) {
    if (cfg.max_connections == 0) {
        return nullptr;
    }
    return std::make_unique<TcpTransport>(cfg);
}

}  // namespace mmo::net
