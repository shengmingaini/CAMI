#pragma once

/// TASK-017 · InventorySystem 公开接口（§7，冻结契约）。
///
/// State Owner（§4）：背包与装备栏的权威写入者是 InventorySystem（Scene 线程内，单 Owner）。
/// 任何增删必须产生标准 Command 与 Event（§8 / §21）。
/// 装备变更触发的属性重算**只能在 Scene 线程内**完成，且必须写 Role 的 from_equipment 层
/// 后 Recompute，禁止直接改 Final（§21）。
///
/// 跨模块约束（§27.2）：通过 RoleSystem 的**公开接口**联动属性，禁止访问其内部数据；
/// 本系统只调用 RoleSystem::FindByPlayer / RecomputeAttributes 与 Character::attrs.from_equipment
///（均为 TASK-016 公开成员）。

#include <cstddef>
#include <unordered_map>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/game/inventory/commands.h"
#include "mmo/game/inventory/inventory.h"
#include "mmo/game/inventory/item.h"
#include "mmo/game/role/role_system.h"  // RoleSystem（装备属性联动）

namespace mmo::game::inventory {

class InventorySystem {
public:
    /// store：物品静态配置；role：属性联动目标（必填，装备必须触发 from_equipment 重算）。
    explicit InventorySystem(const ItemDefStore& store, mmo::game::role::RoleSystem& role) noexcept;

    /// 绑定事件总线（§9：事件在 Tick 的 Event 阶段 Drain 派发）。
    void BindEventBus(core::EventBus& bus) noexcept { events_ = &bus; }

    /// 加入物品（堆叠/占空槽）。背包真满且无法堆叠 → Fail(BUSY)，物品不产生也不丢失（§19）。
    core::Result<std::uint32_t> Add(PlayerId, ItemId def_id, std::uint32_t count,
                                    core::TraceID) noexcept;

    /// 移除指定 guid 的 count 个。count > 持有 → 拒绝（不扣减）；guid 不存在 → Fail(NOT_FOUND)。
    core::Result<std::uint32_t> Remove(PlayerId, ItemGuid, std::uint32_t count,
                                       core::TraceID) noexcept;

    /// 装备（从背包取出放入装备槽）。槽位不匹配 / 等级不足 / 同槽已占用 / 不可装备 → Fail。
    core::Result<void> Equip(PlayerId, ItemGuid, EquipSlot, core::TraceID) noexcept;

    /// 卸下（从装备槽放回背包）。槽位空 → Fail(INVALID_ARGUMENT)。
    core::Result<void> Unequip(PlayerId, EquipSlot, core::TraceID) noexcept;

    /// 耐久损耗。耐久 < 50% 属性减半（可配系数），归零则失效（属性不生效但物品保留，§15.6）。
    core::Result<void> DamageDurability(PlayerId, EquipSlot, std::uint32_t amount,
                                        core::TraceID) noexcept;

    /// 只读视图（禁止返回可变引用，§7）。
    const Inventory* View(PlayerId) const noexcept;
    /// 某玩家已占用槽数（§7 冻结签名）。
    SlotIndex UsedSlots(PlayerId) const noexcept;
    std::size_t PlayerCount() const noexcept { return inv_.size(); }

private:
    void Emit(const auto& ev) noexcept {
        if (events_ != nullptr) (void)events_->Publish(ev);
    }

    /// 重算某玩家的 from_equipment 层（遍历装备槽，含耐久衰减系数）并触发 Role::RecomputeAttributes。
    void RecomputeEquipmentAttrs(PlayerId) noexcept;

    const ItemDefStore& store_;
    mmo::game::role::RoleSystem& role_;
    core::EventBus* events_{nullptr};
    std::unordered_map<PlayerId, Inventory> inv_;
};

}  // namespace mmo::game::inventory
