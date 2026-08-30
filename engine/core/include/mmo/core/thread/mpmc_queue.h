#pragma once

/// MpmcQueue：Vyukov 风格的有界无锁 MPMC 队列。
///
/// 用途（TASK-004 §15.3）：Thread 的任务队列，多生产者多消费者，
/// 队列满时 TryPush 返回 false（由调用方翻译成 ErrorCode::BUSY），
/// **绝不阻塞、绝不丢任务**。
///
/// 算法要点：
///   - 环形数组 + 每格一个 sequence 号，用「seq - pos」的差值判断格子归属；
///   - 容量必须是 2 的幂（用掩码取模，省一次除法）；构造函数自动向上取整；
///   - 入队/出队位置各自独占一个 cache line，避免生产者与消费者互相踩 cache；
///   - 全部用 acquire/release 语义：seq 的 release store 与 acquire load 构成同步边，
///     保证消费者看到完整的 data。
///
/// 约束：T 必须可默认构造 + 可 noexcept 移动赋值（TaskFn 满足）。
///
/// 线程安全：所有方法都可在任意线程并发调用。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace mmo::core {

template <typename T>
class MpmcQueue {
public:
    /// capacity 会被向上取整到 2 的幂（Vyukov 算法要求）。
    explicit MpmcQueue(std::size_t capacity)
        : capacity_(RoundUpPow2(capacity)),
          mask_(capacity_ - 1),
          cells_(nullptr) {
        cells_ = new Cell[capacity_];
        for (std::size_t i = 0; i < capacity_; ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    ~MpmcQueue() { delete[] cells_; }

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    /// 非阻塞入队。成功返回 true；队列满返回 false（不阻塞、不丢任务）。
    bool TryPush(T&& value) noexcept {
        static_assert(std::is_nothrow_move_assignable_v<T>,
                      "MpmcQueue: T 的移动赋值必须 noexcept");
        Cell* cells = cells_;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells[pos & mask_];
            const std::size_t seq = cell.seq.load(std::memory_order_acquire);
            const std::intptr_t dif =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (dif == 0) {
                // 这一格归我：先抢占位置，再写数据
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                                       std::memory_order_relaxed)) {
                    cell.data = std::move(value);
                    cell.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS 失败：pos 已被 compare_exchange_weak 刷新，继续循环
            } else if (dif < 0) {
                return false;  // 队尾追上了队头 —— 队列满
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    /// 非阻塞出队。成功返回 true；队列空返回 false。
    bool TryPop(T& out) noexcept {
        Cell* cells = cells_;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells[pos & mask_];
            const std::size_t seq = cell.seq.load(std::memory_order_acquire);
            const std::intptr_t dif =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                                                       std::memory_order_relaxed)) {
                    out = std::move(cell.data);
                    // 把这一格的 seq 推到「下一轮」，生产者才能再次写入
                    cell.seq.store(pos + mask_ + 1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                return false;  // 队列空
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    std::size_t Capacity() const noexcept { return capacity_; }

    /// 近似深度（并发下瞬时值，只用于指标采集，不用于正确性判断）。
    std::size_t Size() const noexcept {
        const std::size_t head = enqueue_pos_.load(std::memory_order_acquire);
        const std::size_t tail = dequeue_pos_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : 0U;
    }

    bool Empty() const noexcept { return Size() == 0; }

private:
    struct Cell {
        std::atomic<std::size_t> seq;
        T data;
    };

    static constexpr std::size_t kCacheLine = 64;

    static std::size_t RoundUpPow2(std::size_t n) noexcept {
        std::size_t v = (n < 2U) ? 2U : n;
        --v;
        for (std::size_t s = 1; s < sizeof(std::size_t) * 8U; s <<= 1) {
            v |= v >> s;
        }
        return v + 1;
    }

    alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
    std::size_t capacity_;
    std::size_t mask_;
    Cell* cells_;
};

}  // namespace mmo::core
