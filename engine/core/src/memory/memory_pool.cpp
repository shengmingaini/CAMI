// TASK-004 · Core Memory —— 定长块内存池实现
//
// 热路径（拥有者线程）只有「取链表头 / 挂链表头」两步，全程无锁无原子：
//   Allocate  : free_head_ 弹出一个块，写魔数
//   Deallocate: 清魔数，挂回 free_head_
// 冷路径（跨线程归还 / 扩 chunk / 大块）才涉及原子与系统调用。

#include "mmo/core/memory/memory_pool.h"

#include <new>
#include <utility>

namespace mmo::core {

std::size_t MemoryPool::RoundUp(std::size_t n, std::size_t align) noexcept {
    return (n + align - 1) & ~(align - 1);
}

MemoryPool::MemoryPool(std::size_t block_size, std::size_t blocks_per_chunk)
    : usable_(RoundUp(block_size < kMinUsable ? kMinUsable : block_size, 8)),
      stride_(RoundUp(kHeaderBytes + usable_, kBlockAlign)),
      blocks_per_chunk_(blocks_per_chunk == 0 ? 1 : blocks_per_chunk),
      chunk_bytes_(stride_ * blocks_per_chunk_),
      owner_id_(std::this_thread::get_id()),
      remote_(kRemoteCapacity) {}

MemoryPool::~MemoryPool() {
    for (unsigned char* base : chunks_) {
        ::operator delete(static_cast<void*>(base), std::align_val_t(kBlockAlign));
    }
    chunks_.clear();
}

void* MemoryPool::Allocate(std::size_t bytes) {
    if (bytes > usable_) {
        return AllocateLarge(bytes);
    }
    if (free_head_ == nullptr) {
        DrainRemote();
    }
    if (free_head_ == nullptr && !AddChunk()) {
        return nullptr;  // 系统内存耗尽：返回 nullptr，绝不崩溃（§19）
    }

    Header* header = free_head_;
    free_head_ = static_cast<Header*>(header->next);
    --free_depth_;
    header->magic = kPoolMagic;
    header->next = nullptr;
    return PayloadOf(header);
}

void MemoryPool::Deallocate(void* ptr, std::size_t bytes) noexcept {
    if (ptr == nullptr) {
        return;
    }

    Header* header = HeaderOf(ptr);

    if (header->magic == kLargeMagic) {
        header->magic = 0;
        ::operator delete(static_cast<void*>(header), std::align_val_t(kBlockAlign));
        return;
    }
    // bytes 用于一致性校验：声称归还的尺寸超过定长块上限，说明指针与尺寸不匹配。
    if (header->magic != kPoolMagic || bytes > usable_) {
        // 野指针 / 重复释放 / 尺寸对不上：只记指标，绝不崩溃、
        // 绝不去解引用一个来路不明的指针（§19）。
        invalid_frees_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 先清魔数 —— 这样「同一块被归还两次」会被上面的分支拦住，而不是静默地
    // 把同一个块塞进空闲链表两次（后者会在下一次分配时造成两个指针指向同一块内存）。
    header->magic = 0;

    if (std::this_thread::get_id() == owner_id_) {
        header->next = free_head_;
        free_head_ = header;
        ++free_depth_;
        return;
    }

    // 跨线程归还：进有界无锁队列；队列满则退到自旋锁保护的溢出表（有界不等于丢弃）。
    remote_returns_.fetch_add(1, std::memory_order_relaxed);
    void* payload = PayloadOf(header);
    if (!remote_.TryPush(std::move(payload))) {
        overflow_count_.fetch_add(1, std::memory_order_relaxed);
        while (overflow_flag_.test_and_set(std::memory_order_acquire)) {
            // 临界区极短（一次 push_back），自旋即可
        }
        overflow_.push_back(payload);
        overflow_flag_.clear(std::memory_order_release);
    }
}

std::size_t MemoryPool::UsedBytes() const noexcept { return chunks_.size() * chunk_bytes_; }
std::size_t MemoryPool::ChunkSize() const noexcept { return chunk_bytes_; }
std::size_t MemoryPool::BlockSize() const noexcept { return usable_; }
std::size_t MemoryPool::BlockCount() const noexcept {
    return chunks_.size() * blocks_per_chunk_;
}
std::size_t MemoryPool::FreeDepth() const noexcept { return free_depth_; }
std::size_t MemoryPool::RemoteReturns() const noexcept {
    return remote_returns_.load(std::memory_order_relaxed);
}
std::size_t MemoryPool::InvalidFrees() const noexcept {
    return invalid_frees_.load(std::memory_order_relaxed);
}
std::size_t MemoryPool::OverflowCount() const noexcept {
    return overflow_count_.load(std::memory_order_relaxed);
}

bool MemoryPool::AddChunk() {
    void* raw = ::operator new(chunk_bytes_, std::align_val_t(kBlockAlign), std::nothrow);
    if (raw == nullptr) {
        return false;
    }
    unsigned char* base = static_cast<unsigned char*>(raw);
    chunks_.push_back(base);

    // 逆序挂链表：弹出来的地址就是递增的，首次分配具有良好的空间局部性。
    for (std::size_t i = blocks_per_chunk_; i-- > 0;) {
        Header* header = reinterpret_cast<Header*>(base + i * stride_);
        header->magic = 0;
        header->next = free_head_;
        free_head_ = header;
    }
    free_depth_ += blocks_per_chunk_;
    return true;
}

void MemoryPool::DrainRemote() noexcept {
    void* payload = nullptr;
    while (remote_.TryPop(payload)) {
        PushLocal(payload);
    }

    // 溢出表也必须加锁读：它是跨线程写的，无锁读 empty() 是数据竞争。
    while (overflow_flag_.test_and_set(std::memory_order_acquire)) {
        // 自旋等待
    }
    for (void* pending : overflow_) {
        PushLocal(pending);
    }
    overflow_.clear();
    overflow_flag_.clear(std::memory_order_release);
}

void MemoryPool::PushLocal(void* payload) noexcept {
    Header* header = HeaderOf(payload);
    header->magic = 0;
    header->next = free_head_;
    free_head_ = header;
    ++free_depth_;
}

void* MemoryPool::AllocateLarge(std::size_t bytes) {
    void* raw = ::operator new(kHeaderBytes + bytes, std::align_val_t(kBlockAlign),
                               std::nothrow);
    if (raw == nullptr) {
        return nullptr;
    }
    Header* header = static_cast<Header*>(raw);
    header->magic = kLargeMagic;
    header->next = nullptr;
    return PayloadOf(header);
}

MemoryPool::Header* MemoryPool::HeaderOf(void* payload) const noexcept {
    return reinterpret_cast<Header*>(static_cast<unsigned char*>(payload) - kHeaderBytes);
}

void* MemoryPool::PayloadOf(Header* header) noexcept {
    return static_cast<void*>(reinterpret_cast<unsigned char*>(header) + kHeaderBytes);
}

}  // namespace mmo::core
