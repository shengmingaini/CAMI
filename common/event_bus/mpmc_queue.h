#pragma once
// 有界 MPMC（多生产者 / 多消费者）无锁队列 —— CAMI 内部事件总线（ADR-012）的承载队列。
//
// 算法: Dmitry Vyukov 经典 MPMC 环形队列（见 http://www.1024cores.net）。
//   - enqueue / dequeue 全程无锁，仅用原子 CAS / fetch_add；队列非空/未满时为 wait-free。
//   - 每个 cell 带独立 sequence 号，从根本上消除 ABA 问题。
//   - cell 按 64 字节缓存行对齐（alignas(64)），生产者/消费者位置计数器各自独占缓存行，
//     彻底消除伪共享（false sharing），这是高并发吞吐的关键。
//   - 容量必须为 2 的幂，mask = capacity - 1 用位与代替取模。
//   - 内存序: cell.sequence 用 acquire/release 建立生产者↔消费者间的数据可见性；
//     位置计数器用 relaxed（真正的同步点在 cell.sequence，不在位置计数器）。
//
// 适用: 高频事件 / 消息传递热路径。ADR-012 要求内部 EventBus 事件与网络 FlatBuffers
//       消息名分离，本队列即承载前者（如 MobKilledEvent / ItemObtainedEvent / PlayerEnterAreaEvent）。
//
// C++17, header-only。T 需可拷贝（对 uint64_t / std::shared_ptr<Envelope> / 小结构体均廉价）。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>

namespace cami {
namespace common {

template <typename T>
class mpmc_queue {
public:
    explicit mpmc_queue(std::size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          enqueue_pos_(0),
          dequeue_pos_(0) {
        // 容量必须是 2 的幂；非 2 的幂属配置错误（位与取模会错乱）。
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            std::terminate();
        }
        buffer_ = std::make_unique<cell[]>(capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // 含 std::atomic 成员且队列在 map 中就地构造，故禁拷贝、禁移动。
    mpmc_queue(const mpmc_queue&) = delete;
    mpmc_queue& operator=(const mpmc_queue&) = delete;

    // 入队: 成功返回 true；队列满返回 false。热路径无锁。
    bool enqueue(const T& item) {
        cell* c = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            c = &buffer_[pos & mask_];
            std::size_t seq = c->sequence.load(std::memory_order_acquire);
            std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;  // 队列满
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        c->data = item;
        c->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // 出队: 成功返回 true 且 item 被赋值；队列空返回 false。热路径无锁。
    bool dequeue(T& item) {
        cell* c = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            c = &buffer_[pos & mask_];
            std::size_t seq = c->sequence.load(std::memory_order_acquire);
            std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;  // 队列空
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        item = c->data;
        c->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const { return capacity_; }

    // 近似大小（并发下不精确，仅供观测/压测）。
    std::size_t approx_size() const {
        std::intptr_t diff = static_cast<std::intptr_t>(
            enqueue_pos_.load(std::memory_order_relaxed))
            - static_cast<std::intptr_t>(dequeue_pos_.load(std::memory_order_relaxed));
        return diff < 0 ? 0 : static_cast<std::size_t>(diff);
    }

private:
    // alignas(64) 使 cell 大小为 64 的倍数 → 数组元素天然 64 字节间隔且 64 字节对齐，
    // 生产者写 cell[i] 不会与消费者读相邻 cell[i±1] 落在同一缓存行而互相 invalidate。
    struct alignas(64) cell {
        std::atomic<std::size_t> sequence;
        T data;
    };

    std::size_t capacity_;
    std::size_t mask_;
    std::unique_ptr<cell[]> buffer_;

    // 头/尾位置计数器各自独占缓存行，避免生产者推进 enqueue_pos_ 时
    // 因false sharing 拖慢消费者读 dequeue_pos_。
    alignas(64) std::atomic<std::size_t> enqueue_pos_;
    alignas(64) std::atomic<std::size_t> dequeue_pos_;
};

}  // namespace common
}  // namespace cami
