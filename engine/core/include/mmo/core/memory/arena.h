#pragma once

/// Arena：帧内 bump 分配器（TASK-004 §7 / §15.8）。
///
/// 用法：Tick 帧开始时（或帧末）Reset()，帧内所有临时对象都 Push() 出来，
/// 帧结束整块回收 —— **没有逐个析构、没有 free、没有碎片**。
///
/// 设计要点：
///   - Push 就是「对齐 + 指针加法 + 一次边界检查」，实测 < 3ns；
///   - 内存不足时**再申请一个新块并链起来**（不是返回 nullptr），
///     Reset() 只是把 used 归零并回到首个块 —— 已申请的块全部复用，不还给 OS；
///   - 只适合 POD / trivially destructible 的临时数据：Arena **不会**调用析构函数，
///     需要析构的对象请用 ObjectPool。
///
/// 单线程拥有（§4 State Owner），无锁、无原子。

#include <cstddef>
#include <vector>

namespace mmo::core {

class Arena {
public:
    /// bytes：首个块的字节数，也是后续扩容的最小粒度。
    explicit Arena(std::size_t bytes);

    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    /// 分配 bytes 字节，按 align 对齐（align 必须是 2 的幂且 <= 16）。
    /// 参数非法或内存耗尽返回 nullptr（禁止崩溃）。
    void* Push(std::size_t bytes, std::size_t align = 8);

    /// 整块重置：所有已分配区域一次性作废，块内存保留复用。
    void Reset() noexcept;

    // ---- 指标（§20.6） ----
    std::size_t UsedBytes() const noexcept;      // 当前已分配字节数
    std::size_t CapacityBytes() const noexcept;  // 已向 OS 申请的字节数
    std::size_t BlockCount() const noexcept;     // 块数量（>1 说明发生过扩容）

private:
    struct Block {
        unsigned char* base{nullptr};
        std::size_t size{0};
        std::size_t used{0};
    };

    bool AddBlock(std::size_t bytes) noexcept;
    static std::size_t RoundUp(std::size_t n, std::size_t align) noexcept;

    static constexpr std::size_t kBlockAlign = 16;

    std::vector<Block> blocks_;
    std::size_t block_bytes_;
    std::size_t cur_{0};
};

}  // namespace mmo::core
