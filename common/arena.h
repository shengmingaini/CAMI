#pragma once
// ============================================================================
// common/arena.h — 请求级 Arena 分配器 (纯 STL, 零外部依赖)
// ----------------------------------------------------------------------------
// 使用场景: 每个游戏请求处理周期 (如单次消息处理/单帧战斗结算) 分配一个 Arena,
// 所有临时对象 (std::string, std::vector, 消息体) 走 Allocate, 请求处理完
// 整个 Arena 一次性 Reset (O(1) 释放全部页面), 热路径零 malloc/free。
//
// 设计:
//   - 固定页大小 (4KB), 超出按倍数拆分;
//   - 单线程使用 (非线程安全), 通常为栈上对象或 thread_local;
//   - Reset 不释放内存 (页面缓存复用), 只重置游标, 避免反复向 OS 申请;
//   - 对齐: 8 字节对齐 (x86-64 默认), 标准库类型兼容。
//
// [2026-08-12] CAMI 彻底优化白皮书阶段 A — 基础设施重建里程碑 1/4。
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cami {
namespace common {

class Arena {
public:
    static constexpr std::size_t kDefaultPageSize = 4096;

    explicit Arena(std::size_t page_size = kDefaultPageSize)
        : page_size_(page_size) {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // 分配 n 字节 (8 字节对齐), 返回有效指针。
    // 热路径: 通常仅需 page size 内推进游标 (O(1), 无系统调用)。
    void* Allocate(std::size_t n) {
        n = (n + 7) & ~std::size_t{7};  // 8 字节对齐
        if (pages_.empty() || pages_.back().used + n > page_size_) {
            pages_.push_back({std::make_unique<std::uint8_t[]>(page_size_), 0});
        }
        auto& p = pages_.back();
        void* ptr = p.data.get() + p.used;
        p.used += n;
        total_ += n;
        return ptr;
    }

    // 重置所有页面游标 (不释放内存, 页面缓存复用)。
    // 请求处理完调用一次, Arena 可反复使用。
    void Reset() {
        for (auto& p : pages_) p.used = 0;
        total_ = 0;
    }

    // 诊断: 当前分配总量 (Reset 后归零)。
    std::size_t Allocated() const { return total_; }
    // 诊断: 已分配的页面数。
    std::size_t PageCount() const { return pages_.size(); }

private:
    struct Page {
        std::unique_ptr<std::uint8_t[]> data;
        std::size_t used;
    };
    std::vector<Page> pages_;
    std::size_t page_size_;
    std::size_t total_ = 0;
};

}  // namespace common
}  // namespace cami
