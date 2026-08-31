// server/gamenode/inventory/src/item.cpp — TASK-017 §8 物品实例全局唯一 guid
//
// 设计铁律（§21）：物品实例必须有全局唯一、不可复用的 guid（防复制，TASK-030 基础）。
// 单调递增原子计数器：从 1 起，永不回绕、永不复用；0 保留为「无效 guid」。

#include "mmo/game/inventory/item.h"

#include <atomic>

namespace mmo::game::inventory {

ItemGuid NewItemGuid() noexcept {
    // 单例原子计数器；relaxed 足矣（只要求全局唯一单调，不要求跨线程顺序）。
    static std::atomic<std::uint64_t> g_counter{1};
    const ItemGuid g = g_counter.fetch_add(1, std::memory_order_relaxed);
    // 计数器初值 1，fetch_add 后首值即为 1；理论上若回绕到 0（约 2^64 次）属宇宙级事件，
    // 这里不做防御（防御成本高于收益），实践中等价于「永不为 0、永不重复」。
    return g;
}

}  // namespace mmo::game::inventory
