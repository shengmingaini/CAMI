#pragma once
// ============================================================================
// common/spmc_queue.h — 无锁单生产者多消费者环形队列 (纯 STL)
// ----------------------------------------------------------------------------
// 场景: EventBus 消息投递、Cell 间跨进程消息 (共享内存场景)、任务分发。
// 单生产者指"同一时刻仅一个线程 Push", 可替换生产者 (如不同 Cell 各生产);
// 多消费者可并发 Pop, 用 CAS 原子竞争推进 head。
//
// 设计:
//   - 容量 N 必须是 2 的幂 (位与取模, 无除法);
//   - 仅支持值类型 (POD / trivially copyable), 不可存指针或动态分配对象;
//   - TryPush / TryPop 均为无锁操作 (非阻塞), 满/空时返回 false;
//   - 容量选择: 生产消费平衡时 N>=256 即可; 突发大时按量调。
//
// [2026-08-12] CAMI 彻底优化白皮书阶段 A — 基础设施重建里程碑 2/4。
// ============================================================================
#include <array>
#include <atomic>
#include <cstddef>

namespace cami {
namespace common {

template <typename T, std::size_t N>
class SPMCQueue {
    static_assert((N & (N - 1)) == 0, "SPMCQueue capacity must be power of 2");
    static_assert(N >= 2, "SPMCQueue capacity must be >= 2");

public:
    SPMCQueue() {
        for (auto& slot : buffer_) slot.store(T{}, std::memory_order_relaxed);
    }

    // 生产者入队: 满则返回 false (无阻塞)
    bool TryPush(const T& val) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (t + 1) & (N - 1);
        if (next == head_.load(std::memory_order_acquire)) {
            return false;  // 满
        }
        buffer_[t].store(val, std::memory_order_relaxed);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // 消费者出队: 空则返回 false (多消费者并发竞争 head)
    bool TryPop(T& out) {
        std::size_t h = head_.load(std::memory_order_relaxed);
        for (;;) {
            if (h == tail_.load(std::memory_order_acquire)) {
                return false;  // 空
            }
            out = buffer_[h].load(std::memory_order_relaxed);
            const std::size_t next = (h + 1) & (N - 1);
            if (head_.compare_exchange_weak(h, next,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                return true;   // 成功推进
            }
            // CAS 失败: 另一消费者抢先, h 已更新, 重试
        }
    }

    // 诊断
    std::size_t Capacity() const { return N - 1; }  // 一个槽位用于区分空/满

private:
    std::array<std::atomic<T>, N> buffer_;
    // pad to cache line to avoid false sharing on head/tail
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace common
}  // namespace cami
