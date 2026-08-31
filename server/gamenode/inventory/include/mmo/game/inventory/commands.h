#pragma once

/// TASK-017 · 标准 Command 与 Event（§7 / §8）。
///
/// 约束（§8 / §21）：四种修改操作（Add / Remove / Equip / Unequip）必须产生对应 Event；
/// 事件缺失即为 bug（测试断言事件数 == 操作数）。事件值类型 ≤ 32B、nothrow move、alignof ≤ 8
/// （EventBus 内联预算），全部 POD、无所有权。
///
/// 忠诚于「留痕」：InventorySystem 的每个公开方法即对应 Command 的处理器，
/// 内部构造 Command 结构用于审计/可观测，并**发布** Event（Command 是入站请求侧，不回发）。

#include <cstdint>

#include "mmo/core/log/trace_id.h"        // TraceID / RequestID
#include "mmo/game/role/character.h"        // PlayerId（TASK-016 复用 mmo::game::PlayerId）
#include "mmo/game/inventory/item.h"        // ItemGuid / ItemId / EquipSlot

namespace mmo::game::inventory {

using PlayerId = mmo::game::PlayerId;

/// ---- Command（入站请求，InventorySystem 方法即其处理器）----

struct AddItemCommand {
    core::RequestID request_id{0};
    PlayerId player{0};
    ItemId def_id{0};
    std::uint32_t count{0};
    core::TraceID trace{0};
};

struct RemoveItemCommand {
    core::RequestID request_id{0};
    PlayerId player{0};
    ItemGuid guid{kInvalidItemGuid};
    std::uint32_t count{0};
    core::TraceID trace{0};
};

struct EquipCommand {
    core::RequestID request_id{0};
    PlayerId player{0};
    ItemGuid guid{kInvalidItemGuid};
    EquipSlot slot{EquipSlot::Count};
    core::TraceID trace{0};
};

struct UnequipCommand {
    core::RequestID request_id{0};
    PlayerId player{0};
    EquipSlot slot{EquipSlot::Count};
    core::TraceID trace{0};
};

/// ---- Event（出站，InventorySystem 发布）----

/// 物品进入背包（source：0=掉落 1=任务 2=合成 3=购买；本任务统一用 0）。
struct ItemAdded {
    ItemGuid guid{kInvalidItemGuid};
    ItemId def_id{0};
    std::uint32_t count{0};
    std::uint8_t source{0};
};

/// 物品移出背包（reason：0=使用 1=丢弃 2=出售 3=销毁；本任务用 0/1）。
struct ItemRemoved {
    ItemGuid guid{kInvalidItemGuid};
    ItemId def_id{0};
    std::uint32_t count{0};
    std::uint8_t reason{0};
};

/// 装备（attr_delta = 该装备有效属性贡献的 L1 范数变化，可观测标量，详见 inventory_system.cpp）。
struct ItemEquipped {
    ItemGuid guid{kInvalidItemGuid};
    EquipSlot slot{EquipSlot::Count};
    std::int64_t attr_delta{0};
};

struct ItemUnequipped {
    ItemGuid guid{kInvalidItemGuid};
    EquipSlot slot{EquipSlot::Count};
    std::int64_t attr_delta{0};
};

/// 耐久变化（from→to；归零表示失效但物品保留）。
struct DurabilityChanged {
    ItemGuid guid{kInvalidItemGuid};
    std::uint32_t from{0};
    std::uint32_t to{0};
};

static_assert(sizeof(ItemAdded) <= 32);
static_assert(sizeof(ItemRemoved) <= 32);
static_assert(sizeof(ItemEquipped) <= 32);
static_assert(sizeof(ItemUnequipped) <= 32);
static_assert(sizeof(DurabilityChanged) <= 32);

}  // namespace mmo::game::inventory
