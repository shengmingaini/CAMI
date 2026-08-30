#pragma once

/// TASK-008 §8 Data Model / §15.2 —— ConnectionId 分配（SlotMap + 防 ABA）与连接对象。
///
/// 设计要点：
///   - ConnectionId 编码：(generation << 32) | slot_index，同 slot 复用后 generation 递增，
///     旧 id 必然失效（防 ABA，杜绝「回收后误发旧连接」）；
///   - 分配器维护全局单调序号，保证新分配 id 严格大于此前所有 id；
///   - TcpConnection：发送缓冲（Buffer）满返回 BUSY（§21 禁止无界增长）；
///     发送路径加锁（业务线程可安全 Send），接收路径由 IO 线程独占（无锁）；
///     Send 自动加 4B 长度前缀（§8 Packet），与 Recv 侧剥前缀对称；
///   - 优雅关闭：Graceful 先排空发送缓冲再 FIN；非优雅直接 RST/关闭。
///   - 本头文件零平台依赖（SocketHandle 用 uintptr_t 承载，winsock 细节在 src）。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/net/buffer.h"
#include "mmo/net/transport.h"

namespace mmo::net {

/// 平台无关的 socket 句柄载体（64-bit Windows SOCKET = UINT_PTR；Linux int）。
using SocketHandle = std::uintptr_t;
inline constexpr SocketHandle kInvalidSocket = ~SocketHandle{0};  // INVALID_SOCKET 同值

/// 连接状态机：Open -> Closing（等发送排空）-> Closed。
enum class ConnState : std::uint8_t {
    Open = 0,
    Closing = 1,  // 已请求优雅关闭，待发送缓冲排空
    Closed = 2,
};

/// ConnectionId 分配器：SlotMap + generation 防 ABA + 全局单调。
class ConnectionIdAllocator {
public:
    explicit ConnectionIdAllocator(std::size_t capacity);

    ConnectionIdAllocator(const ConnectionIdAllocator&) = delete;
    ConnectionIdAllocator& operator=(const ConnectionIdAllocator&) = delete;
    ConnectionIdAllocator(ConnectionIdAllocator&&) noexcept = default;
    ConnectionIdAllocator& operator=(ConnectionIdAllocator&&) noexcept = default;

    /// 分配一个新 id（复用空闲 slot，generation 递增）。容量耗尽返回 nullopt。
    std::optional<ConnectionId> Allocate();
    /// 归还 id 对应的 slot（幂等：无效 id 静默忽略）。
    void Release(ConnectionId id) noexcept;
    /// id 当前是否有效（slot 越界 / generation 不匹配 / 未占用均返回 false）。
    bool IsValid(ConnectionId id) const noexcept;
    /// 当前已占用连接数。
    std::size_t Size() const noexcept { return in_use_; }
    std::size_t Capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        std::uint32_t generation{0};
        bool in_use{false};
    };
    static constexpr std::uint32_t kSlotBits = 32;
    static constexpr std::uint64_t kSlotMask = (std::uint64_t{1} << kSlotBits) - 1;

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;  // 空闲 slot 栈
    std::size_t in_use_{0};
    std::uint64_t next_serial_{1};     // 全局单调：新 id 必须 >= 此值
};

/// 单连接对象。接收缓冲仅 IO 线程访问；发送缓冲加锁（业务线程可并发 Send）。
class TcpConnection final : public IConnection {
public:
    TcpConnection(ConnectionId id, SocketHandle fd, std::string remote_addr,
                  std::size_t recv_cap, std::size_t send_cap);
    ~TcpConnection() override;

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // ---- IConnection ----
    ConnectionId Id() const noexcept override { return id_; }
    core::Result<void> Send(std::span<const std::uint8_t> data) override;
    core::Result<void> Close(CloseReason reason) noexcept override;
    std::string_view RemoteAddr() const noexcept override { return remote_addr_; }
    ConnectionStats Stats() const noexcept override;

    // ---- IO 线程专用 ----
    SocketHandle Socket() const noexcept { return fd_; }
    /// 接收缓冲（仅 IO 线程读写）。
    Buffer& RecvBuffer() noexcept { return recv_buf_; }
    /// 发送缓冲（IO 线程读；业务线程经 Send 写）。
    Buffer& SendBuffer() noexcept { return send_buf_; }
    /// 发送缓冲是否非空（IO 线程决定是否注册可写事件）。
    bool HasPendingSend() const noexcept {
        std::lock_guard<std::mutex> lk(send_mu_);
        return send_buf_.Size() > 0;
    }
    /// IO 线程每轮 Poll 后尝试排空发送缓冲；排空后若处于 Closing 则触发优雅关闭。
    /// 返回 true = 发送缓冲已空。
    bool DrainSend() noexcept;
    /// 当前状态（原子读）。
    ConnState State() const noexcept { return state_.load(std::memory_order_acquire); }
    /// 已请求的关闭原因（仅在 CloseRequested() 后有意义）。
    CloseReason Reason() const noexcept { return close_reason_; }
    /// 是否已标记关闭（Closing 或 Closed）。
    bool CloseRequested() const noexcept {
        return state_.load(std::memory_order_acquire) != ConnState::Open;
    }
    /// 标记为已关闭（IO 线程回收时调用）。
    void MarkClosed() noexcept { state_.store(ConnState::Closed, std::memory_order_release); }

    /// 连接级指标原子累计（供 IO 线程热路径调用）。
    void AddBytesIn(std::uint64_t n) noexcept {
        stats_.bytes_in.fetch_add(n, std::memory_order_relaxed);
    }
    void AddBytesOut(std::uint64_t n) noexcept {
        stats_.bytes_out.fetch_add(n, std::memory_order_relaxed);
    }
    void AddPacketsIn(std::uint64_t n) noexcept {
        stats_.packets_in.fetch_add(n, std::memory_order_relaxed);
    }
    void AddPacketsOut(std::uint64_t n) noexcept {
        stats_.packets_out.fetch_add(n, std::memory_order_relaxed);
    }
    void AddError() noexcept {
        stats_.error_count.fetch_add(1, std::memory_order_relaxed);
    }

private:
    ConnectionId id_;
    SocketHandle fd_;
    std::string remote_addr_;

    // 接收路径：IO 线程独占，无锁
    Buffer recv_buf_;
    // 发送路径：业务线程写 / IO 线程读，锁保护
    mutable std::mutex send_mu_;
    Buffer send_buf_;

    std::atomic<ConnState> state_{ConnState::Open};
    CloseReason close_reason_{CloseReason::Graceful};

    struct AtomicStats {
        std::atomic<std::uint64_t> bytes_in{0};
        std::atomic<std::uint64_t> bytes_out{0};
        std::atomic<std::uint64_t> packets_in{0};
        std::atomic<std::uint64_t> packets_out{0};
        std::atomic<std::uint64_t> error_count{0};
    };
    AtomicStats stats_;
};

// ---------------------------------------------------------------------------
// ConnectionIdAllocator 实现（header-only）
// ---------------------------------------------------------------------------

inline ConnectionIdAllocator::ConnectionIdAllocator(std::size_t capacity)
    : slots_(capacity) {
    free_.reserve(capacity);
    for (std::uint32_t i = 0; i < capacity; ++i) {
        free_.push_back(i);
    }
}

inline std::optional<ConnectionId> ConnectionIdAllocator::Allocate() {
    if (free_.empty()) {
        return std::nullopt;
    }
    const std::uint32_t idx = free_.back();
    free_.pop_back();
    auto& slot = slots_[idx];
    slot.in_use = true;
    ++slot.generation;
    // 保证全局单调：必要时抬高 generation（最多抬升若干次，足够覆盖容量）
    std::uint64_t cand =
        (std::uint64_t{slot.generation} << kSlotBits) | std::uint64_t{idx};
    if (cand < next_serial_) {
        const std::uint64_t delta = next_serial_ - cand;
        slot.generation += static_cast<std::uint32_t>((delta + kSlotMask) >> kSlotBits);
        cand = (std::uint64_t{slot.generation} << kSlotBits) | std::uint64_t{idx};
    }
    next_serial_ = cand + 1;
    ++in_use_;
    return ConnectionId{cand};
}

inline void ConnectionIdAllocator::Release(ConnectionId id) noexcept {
    const std::uint32_t idx = static_cast<std::uint32_t>(id & kSlotMask);
    if (idx >= slots_.size()) {
        return;
    }
    auto& slot = slots_[idx];
    if (!slot.in_use) {
        return;
    }
    const std::uint32_t gen = static_cast<std::uint32_t>(id >> kSlotBits);
    if (gen != slot.generation) {
        return;  // 旧 id，静默忽略
    }
    slot.in_use = false;
    --in_use_;
    free_.push_back(idx);
}

inline bool ConnectionIdAllocator::IsValid(ConnectionId id) const noexcept {
    const std::uint32_t idx = static_cast<std::uint32_t>(id & kSlotMask);
    if (idx >= slots_.size()) {
        return false;
    }
    const auto& slot = slots_[idx];
    if (!slot.in_use) {
        return false;
    }
    const std::uint32_t gen = static_cast<std::uint32_t>(id >> kSlotBits);
    return gen == slot.generation;
}

}  // namespace mmo::net
