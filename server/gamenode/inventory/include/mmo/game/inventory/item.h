#pragma once

/// TASK-017 · 物品定义与实例（§7 / §8）。
///
/// 设计铁律（§21）：物品实例必须有全局唯一 guid（防复制，TASK-030 基础）；
/// ItemDef 是**静态配置、只读**，属性加成在配置里，代码内零硬编码（§20.6）。

#include <array>
#include <cstdint>
#include <string_view>

#include "mmo/game/role/attribute.h"  // AttrType / kAttrCount（装备影响属性）

namespace mmo::game::inventory {

using ItemId = std::uint64_t;    // 物品定义 id（配置固定）
using ItemGuid = std::uint64_t;  // 物品实例 id（服务器生成，全局唯一、不可复用）
using SlotIndex = std::uint16_t;

inline constexpr ItemGuid kInvalidItemGuid = 0;

/// 装备槽位（§7）。位掩码用于「一个物品可装哪些槽」。
enum class EquipSlot : std::uint8_t {
    Head = 0,
    Chest,
    Hands,
    Legs,
    Feet,
    MainHand,
    OffHand,
    Ring1,
    Ring2,
    Neck,
    Count,  // 槽位数（非有效槽）
};

inline constexpr std::size_t kEquipSlotCount = static_cast<std::size_t>(EquipSlot::Count);

/// 槽位位掩码（EquipCommand 的 slot 校验依据）。
inline constexpr std::uint16_t EquipBit(EquipSlot s) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<std::uint32_t>(s));
}

/// 静态物品定义（只读配置；name 为 string_view，生命周期由 ItemDefStore 保证）。
struct ItemDef {
    ItemId def_id{0};
    std::string_view name;        // 指向 ItemDefStore 内持有的 string
    std::uint32_t max_stack{1};   // >1 才可堆叠；装备类固定 1
    std::uint8_t item_type{0};    // 0=消耗品(不可装备)；其余见 EquipSlot 语义
    std::uint32_t required_level{0};
    std::uint32_t max_durability{0};  // 0=无耐久（消耗品/部分装备）
    std::uint16_t equip_slots{0};    // 允许装备的槽位位掩码；0=不可装备
    std::array<std::int64_t, role::kAttrCount> attr_bonus{};  // 装备提供的属性加成（按 AttrType 下标）
};

/// 物品实例（背包/装备栏里的一个堆叠或单件）。
/// sizeof = 8(def_id) + 4(count) + 4(durability) + 8(guid) = 24B（§22 单物品 < 64B）。
struct ItemStack {
    ItemId def_id{0};
    std::uint32_t count{1};
    std::uint32_t durability{0};  // 当前耐久（0=失效但物品保留，§15.6）
    ItemGuid guid{kInvalidItemGuid};
};

/// 生成全局唯一物品实例 guid（单调递增原子，100 万次无重复）。
/// 装备类物品强制唯一；堆叠物品每 stack 一个 guid。
ItemGuid NewItemGuid() noexcept;

}  // namespace mmo::game::inventory
