#pragma once

/// ObjectPool：分块定长对象池（TASK-004 §7 / §15.6 / §21）。
///
/// 设计要点：
///   - **单线程拥有**（§4 State Owner）：每 Worker 独占一份，禁止跨线程共享同一个池。
///     因此热路径 Acquire / Release 全程无锁、无原子操作 —— 只有指针读写和向量 pop/push。
///   - **分块扩容**：每次向 OS 要一整块 `Chunk` 个对象的连续内存，避免频繁小分配；
///     Release 只析构对象、**不把内存还给 OS**（§8 Data Model），下次 Acquire 直接复用。
///   - **prewarm**：构造时就备好槽位，让第一个 Acquire 不触发分配器（Tick 内禁止大分配）。
///   - **Acquire 永不返回 nullptr**（内存不足时由 `new` 抛出前先崩在分配器，
///     这是「池耗尽即扩容」的既定语义；真正需要 nullptr 语义的用 MemoryPool）。
///
/// 析构语义：池析构时会析构所有**仍被借出**的对象（用空闲表反查活跃槽位），
/// 因此不会静默泄漏对象内部资源。这一步只发生在析构，热路径零成本。

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace mmo::core {

template <typename T, std::size_t Chunk = 4096>
class ObjectPool {
public:
    static_assert(Chunk > 0, "ObjectPool: Chunk must be > 0");
    static_assert(std::is_default_constructible_v<T>,
                  "ObjectPool: T 必须可默认构造（Acquire() 无参构造）");

    /// prewarm：预先准备的槽位数（会向上取整到 Chunk 的整数倍）。
    explicit ObjectPool(std::size_t prewarm = 0) {
        while (Capacity() < prewarm) {
            AddChunk();
        }
    }

    ~ObjectPool() { DestroyLiveObjects(); }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /// 取一个对象（默认构造）。池空时自动扩容。
    T* Acquire() {
        if (free_.empty()) {
            AddChunk();
        }
        T* slot = free_.back();
        free_.pop_back();
        ::new (static_cast<void*>(slot)) T();
        ++in_use_;
        return slot;
    }

    /// 带参构造版本：避免「先默认构造再赋值」的二次开销。
    template <typename... Args>
    T* Acquire(Args&&... args) {
        static_assert(std::is_constructible_v<T, Args...>,
                      "ObjectPool: T 不能用给定实参构造");
        if (free_.empty()) {
            AddChunk();
        }
        T* slot = free_.back();
        free_.pop_back();
        ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
        ++in_use_;
        return slot;
    }

    /// 归还。对象析构但内存留在池里。nullptr 与重复归还是未定义行为（单线程拥有，不做校验）。
    void Release(T* ptr) noexcept {
        if (ptr == nullptr) {
            return;
        }
        ptr->~T();
        free_.push_back(ptr);
        --in_use_;
    }

    // ---- 指标（§20.6） ----
    std::size_t Capacity() const noexcept { return chunks_.size() * Chunk; }
    std::size_t InUse() const noexcept { return in_use_; }
    std::size_t FreeCount() const noexcept { return free_.size(); }
    std::size_t ChunkCount() const noexcept { return chunks_.size(); }

private:
    struct ChunkBlock {
        // 原始存储：Acquire 时 placement-new 构造，Release 时显式析构。
        alignas(T) unsigned char bytes[sizeof(T) * Chunk];
    };

    void AddChunk() {
        auto block = std::make_unique<ChunkBlock>();
        unsigned char* base = block->bytes;
        chunks_.push_back(std::move(block));
        free_.reserve(free_.size() + Chunk);
        for (std::size_t i = 0; i < Chunk; ++i) {
            free_.push_back(reinterpret_cast<T*>(base + i * sizeof(T)));
        }
    }

    /// 析构仍被借出的对象：空闲表之外的槽位就是活跃的。
    /// 只发生在池析构，O(n log n)（排序空闲表 + 全量扫描），热路径无开销。
    void DestroyLiveObjects() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            std::vector<T*> sorted(free_.begin(), free_.end());
            std::sort(sorted.begin(), sorted.end());
            for (auto& block : chunks_) {
                unsigned char* base = block->bytes;
                for (std::size_t i = 0; i < Chunk; ++i) {
                    T* slot = reinterpret_cast<T*>(base + i * sizeof(T));
                    if (!std::binary_search(sorted.begin(), sorted.end(), slot)) {
                        slot->~T();
                    }
                }
            }
        }
        (void)in_use_;
    }

    std::vector<std::unique_ptr<ChunkBlock>> chunks_;
    std::vector<T*> free_;
    std::size_t in_use_{0};
};

}  // namespace mmo::core
