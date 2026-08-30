// TASK-004 · Core Memory —— 帧内 bump 分配器实现
//
// 热路径 Push 只有三步：对齐 offset、边界检查、指针加法。
// 空间不够时扩容（冷路径）。Reset 只是把每个块的 used 归零 —— 不还给 OS，
// 这样下一帧从同一个地址重新开始，缓存是热的。

#include "mmo/core/memory/arena.h"

#include <new>

namespace mmo::core {

std::size_t Arena::RoundUp(std::size_t n, std::size_t align) noexcept {
    return (n + align - 1) & ~(align - 1);
}

Arena::Arena(std::size_t bytes)
    : block_bytes_(bytes == 0 ? 4096 : bytes) {
    // 构造失败不抛：AddBlock 用 nothrow，申请不到就得到一个空 Arena（Push 返回 nullptr）。
    (void)AddBlock(block_bytes_);
}

Arena::~Arena() {
    for (Block& block : blocks_) {
        if (block.base != nullptr) {
            ::operator delete(static_cast<void*>(block.base),
                              std::align_val_t(kBlockAlign));
        }
    }
    blocks_.clear();
}

void* Arena::Push(std::size_t bytes, std::size_t align) {
    // align 必须是 2 的幂，且不能超过块的对齐保证（否则无法兑现调用方的对齐要求）。
    if (align == 0 || (align & (align - 1)) != 0 || align > kBlockAlign) {
        return nullptr;
    }

    for (;;) {
        Block& block = blocks_[cur_];
        const std::size_t offset = RoundUp(block.used, align);
        if (offset <= block.size && block.size - offset >= bytes) {
            void* result = static_cast<void*>(block.base + offset);
            block.used = offset + bytes;
            return result;
        }
        // 当前块放不下：扩容。新块至少能装下这次请求。
        const std::size_t want = (bytes + align > block_bytes_) ? (bytes + align) : block_bytes_;
        if (!AddBlock(want)) {
            return nullptr;  // 系统内存耗尽：返回 nullptr，绝不崩溃（§19）
        }
    }
}

void Arena::Reset() noexcept {
    for (Block& block : blocks_) {
        block.used = 0;
    }
    cur_ = 0;
}

std::size_t Arena::UsedBytes() const noexcept {
    std::size_t total = 0;
    for (const Block& block : blocks_) {
        total += block.used;
    }
    return total;
}

std::size_t Arena::CapacityBytes() const noexcept {
    std::size_t total = 0;
    for (const Block& block : blocks_) {
        total += block.size;
    }
    return total;
}

std::size_t Arena::BlockCount() const noexcept { return blocks_.size(); }

bool Arena::AddBlock(std::size_t bytes) noexcept {
    const std::size_t size = RoundUp(bytes, kBlockAlign);
    void* raw = ::operator new(size, std::align_val_t(kBlockAlign), std::nothrow);
    if (raw == nullptr) {
        return false;
    }
    Block block;
    block.base = static_cast<unsigned char*>(raw);
    block.size = size;
    block.used = 0;
    blocks_.push_back(block);
    cur_ = blocks_.size() - 1;
    return true;
}

}  // namespace mmo::core
