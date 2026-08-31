#pragma once

/// TASK-017 · 背包容器与物品配置表（§7 / §15）。
///
/// Inventory：定长槽位数组 + 空闲槽栈，禁止无界增长（§21）。
/// 装备栏与背包分离：Equip 把物品从背包整体移到装备槽（保留 guid 与 durability）；
/// Unequip 放回背包（用原 guid，保持物品身份）。
/// ItemDefStore：从 config/gameplay/items/*.json 加载静态物品定义（配置化，§20.6）。

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/game/inventory/item.h"

namespace mmo::game::inventory {

inline constexpr SlotIndex kMaxInventorySlots = 100;  // §22：单玩家背包 < 8KB（100 槽 × 24B = 2.4KB）

/// 单个玩家的背包 + 装备栏（§15.5 装备变更触发的属性重算由 InventorySystem 在 Scene 线程完成）。
class Inventory {
public:
    /// 加入新物品的结果（用于事件留痕：返回实际加入量与首个新栈 guid）。
    struct AddOutcome {
        ItemGuid guid{kInvalidItemGuid};  // 首个新栈 guid（堆叠进已有栈时为该栈 guid）
        std::uint32_t added{0};           // 实际加入数量
    };

    /// 加入新物品（堆叠优先；装备类 max_stack=1 强制独占空槽）。
    /// 返回实际加入量与新栈 guid；背包真满且无法堆叠 → Fail(BUSY)，物品不产生（§19）。
    core::Result<AddOutcome> Add(const ItemDef& def, std::uint32_t count) noexcept;

    /// 把已有（携带 guid/durability 的）物品实例加回背包（卸下装备用），保留身份。
    /// 背包真满 → Fail(BUSY)，物品不丢失（调用方据此中止 Unequip）。
    core::Result<std::uint32_t> AddExisting(const ItemDef& def, const ItemStack& st) noexcept;

    /// 移除指定 guid 的物品 count 个。count > 持有 → 拒绝（不扣减，防负数，§19）。
    /// 返回实际移除数；guid 不存在 → Fail(NOT_FOUND)。
    core::Result<std::uint32_t> Remove(ItemGuid guid, std::uint32_t count) noexcept;

    const ItemStack* At(SlotIndex i) const noexcept { return (i < kMaxInventorySlots) ? &bag_[i] : nullptr; }
    ItemStack* At(SlotIndex i) noexcept { return (i < kMaxInventorySlots) ? &bag_[i] : nullptr; }

    SlotIndex Find(ItemGuid guid) const noexcept;
    bool Contains(ItemGuid guid) const noexcept { return Find(guid) != kMaxInventorySlots; }
    SlotIndex UsedSlots() const noexcept { return used_; }

    /// 装备栏（存完整 ItemStack，保留 durability）。guid==0 表示空槽。
    const ItemStack& EquippedStack(EquipSlot s) const noexcept {
        return equipped_[static_cast<std::size_t>(s)];
    }
    ItemStack& EquippedStack(EquipSlot s) noexcept {
        return equipped_[static_cast<std::size_t>(s)];
    }
    ItemGuid Equipped(EquipSlot s) const noexcept { return equipped_[static_cast<std::size_t>(s)].guid; }
    void SetEquipped(EquipSlot s, const ItemStack& st) noexcept {
        equipped_[static_cast<std::size_t>(s)] = st;
    }
    void ClearEquipped(EquipSlot s) noexcept { equipped_[static_cast<std::size_t>(s)] = ItemStack{}; }
    bool IsEquipped(ItemGuid guid) const noexcept;

    /// 背包总物品数（所有 stack 的 count 之和）。
    std::uint64_t TotalCount() const noexcept;

private:
    /// 分配空槽：优先复用 free_ 栈，否则用 next_ 线性扩张；满 → 返回 kMaxInventorySlots。
    SlotIndex AllocateSlot() noexcept {
        if (!free_.empty()) { SlotIndex i = free_.back(); free_.pop_back(); return i; }
        if (next_ < kMaxInventorySlots) return next_++;
        return kMaxInventorySlots;
    }

    std::array<ItemStack, kMaxInventorySlots> bag_{};
    std::array<ItemStack, kEquipSlotCount> equipped_{};
    SlotIndex used_{0};   // 已占用槽数
    SlotIndex next_{0};   // 已分配过的最高索引（紧凑布局）
    std::vector<SlotIndex> free_;  // 空闲槽位栈（O(1) 占用/回收）
};

/// 物品定义表（只读配置；name 由本表持有，ItemDef::name 指向它）。
class ItemDefStore {
public:
    /// 从目录加载所有 *.json（每个文件可是一件物品或物品数组）。
    /// 文件缺失/解析失败/重复 def_id → Fail。多文件场景不依赖 core::ConfigManager 的全局快照
    /// （同键合并会冲突），故用受限 JSON 解析（扁平对象 + 顶层数组）。
    core::Result<void> LoadDir(const char* dir);

    const ItemDef* Lookup(ItemId def_id) const noexcept;
    bool Contains(ItemId def_id) const noexcept { return Lookup(def_id) != nullptr; }
    std::size_t Size() const noexcept { return defs_.size(); }

    /// 内部存储单元（name 实际持有，def.name 指向它；解析器在 .cpp 内使用）。
    struct StoredDef {
        std::string name;  // 实际存储
        ItemDef def;        // def.name 指向 name
    };

private:
    std::unordered_map<ItemId, StoredDef> defs_;
};

}  // namespace mmo::game::inventory
