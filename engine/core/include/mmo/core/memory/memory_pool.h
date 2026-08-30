#pragma once

/// MemoryPool：定长块内存池（TASK-004 §7 / §15.7 / §19）。
///
/// 结构：**thread_local 空闲链表 + 全局后备 + 有界跨线程归还队列**。
///   - 池由一个线程拥有（§4 State Owner）。拥有者线程的 Allocate / Deallocate
///     只在侵入式空闲链表上做指针操作，无锁、无原子、无系统调用。
///   - 其他线程归还的块进 `MpmcQueue` 有界队列（§19「跨线程归还有界」），
///     队列满时退到自旋锁保护的溢出表 —— 永不崩溃、永不双释放。
///     拥有者线程在空闲链表见底时把这两处批量收回（DrainRemote）。
///   - 超过定长块上限的请求走「大块通道」：带同样 16 字节头的系统分配，
///     因此 Allocate / Deallocate 对调用方是统一语义，且大块可由任意线程归还。
///
/// 每块带一个 16 字节头（魔数 + 空闲链表 next）。魔数用于区分池内块 / 大块，
/// 并在归还时清零 —— 于是**重复释放会被识别成野指针**，只记指标、不崩溃。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "mmo/core/thread/mpmc_queue.h"

namespace mmo::core {

class MemoryPool {
public:
    /// block_size：单块可用字节上限，会向上取整到 8 的倍数（最小 8）。
    /// blocks_per_chunk：每次向 OS 申请的块数（决定 ChunkSize()）。
    explicit MemoryPool(std::size_t block_size, std::size_t blocks_per_chunk = 1024);

    ~MemoryPool();

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    /// 分配至少 bytes 字节（16 字节对齐）。bytes 超过 BlockSize() 时走大块通道。
    /// 失败返回 nullptr（系统分配器抛 bad_alloc 之前不会有任何「悄悄成功」的假象）。
    void* Allocate(std::size_t bytes);

    /// 归还。bytes 用于校验大块；池内块忽略它。
    /// 野指针 / 双释放：记 InvalidFrees 指标并安全返回，不崩溃（§19）。
    void Deallocate(void* ptr, std::size_t bytes) noexcept;

    // ---- 指标（§20.6 供 TASK-039 采集） ----
    std::size_t UsedBytes() const noexcept;      // 已向 OS 提交的字节数
    std::size_t ChunkSize() const noexcept;      // 单个 chunk 的字节数
    std::size_t BlockSize() const noexcept;      // 单块可用字节数（不含头部）
    std::size_t BlockCount() const noexcept;     // 已切分出的块总数
    std::size_t FreeDepth() const noexcept;      // 拥有者线程空闲链表深度
    std::size_t RemoteReturns() const noexcept;  // 跨线程归还累计次数
    std::size_t InvalidFrees() const noexcept;   // 野指针 / 双释放拦截次数
    std::size_t OverflowCount() const noexcept;  // 跨线程队列满、退到溢出表的次数

private:
    struct Header {
        std::uint64_t magic;
        void* next;
    };

    static constexpr std::size_t kBlockAlign = 16;
    static constexpr std::size_t kHeaderBytes = 16;  // 必须 >= sizeof(Header)
    static constexpr std::size_t kMinUsable = 8;
    static constexpr std::uint64_t kPoolMagic = 0x4D4D4F5F504F4F4CULL;   // "MMO_POOL"
    static constexpr std::uint64_t kLargeMagic = 0x4D4D4F5F4C524745ULL;  // "MMO_LRGE"
    static constexpr std::size_t kRemoteCapacity = 4096;

    bool AddChunk();
    void DrainRemote() noexcept;
    void PushLocal(void* payload) noexcept;
    void* AllocateLarge(std::size_t bytes);
    Header* HeaderOf(void* payload) const noexcept;
    static void* PayloadOf(Header* header) noexcept;
    static std::size_t RoundUp(std::size_t n, std::size_t align) noexcept;

    std::size_t usable_;
    std::size_t stride_;
    std::size_t blocks_per_chunk_;
    std::size_t chunk_bytes_;

    std::thread::id owner_id_;
    Header* free_head_{nullptr};
    std::size_t free_depth_{0};
    std::vector<unsigned char*> chunks_;

    // 跨线程归还路径（冷路径）：有界无锁队列 + 溢出表
    MpmcQueue<void*> remote_;
    std::vector<void*> overflow_;
    std::atomic_flag overflow_flag_;  // C++20 起默认初始化即为 clear 状态

    std::atomic<std::size_t> remote_returns_{0};
    std::atomic<std::size_t> invalid_frees_{0};
    std::atomic<std::size_t> overflow_count_{0};
};

}  // namespace mmo::core
