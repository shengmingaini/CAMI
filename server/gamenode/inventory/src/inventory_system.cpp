// server/gamenode/inventory/src/inventory_system.cpp — TASK-017 §7 / §15 背包系统
//
// State Owner（§4）：背包与装备栏的权威写入者是 InventorySystem（Scene 线程内，单 Owner）。
// 所有增删必须产生标准 Command 与 Event（§8 / §21）。
// 装备变更触发属性重算：**只在 Scene 线程内**完成，且必须写 Role 的 from_equipment 层后
// Recompute，禁止直接改 Final（§21）。跨模块只经 RoleSystem 公开接口联动（§27.2）。
//
// attr_delta 务实落地：事件 ≤32B 约束下无法直接塞 11 维 int64 数组，故用「该装备有效属性
// 贡献的 L1 范数变化」标量（Equip 为正、Unequip 为负），可观测、可断言。

#include "mmo/game/inventory/inventory_system.h"

#include <algorithm>
#include <cstdint>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"

namespace mmo::game::inventory {

namespace {

core::Error NotFound(const char* what) {
    return core::Error(core::ErrorCode::NOT_FOUND, what, core::domain::kCore);
}
core::Error BadArg(const char* what) {
    return core::Error(core::ErrorCode::INVALID_ARGUMENT, what, core::domain::kCore);
}

// 耐久衰减系数（分子，分母固定 2）：0=失效 / 1=减半(<50%) / 2=全额。
int DurabilityFactorN(std::uint32_t max_dur, std::uint32_t cur) noexcept {
    if (max_dur == 0) return 2;        // 无耐久装备：全额生效
    if (cur == 0) return 0;            // 耐久归零：失效（属性不生效，物品保留）
    if (cur * 2 < max_dur) return 1;   // <50%：减半
    return 2;                          // >=50%：全额
}

// 该装备「有效属性贡献」的 L1 范数（可观测标量）。
std::int64_t EffectiveContributionL1(const ItemDef& def, std::uint32_t durability) noexcept {
    const int fn = DurabilityFactorN(def.max_durability, durability);
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < role::kAttrCount; ++i) {
        const std::int64_t eff = def.attr_bonus[i] * fn / 2;
        sum += (eff >= 0) ? eff : -eff;
    }
    return sum;
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 事件总线
// ---------------------------------------------------------------------------

InventorySystem::InventorySystem(const ItemDefStore& store, role::RoleSystem& role) noexcept
    : store_(store), role_(role) {}

// ---------------------------------------------------------------------------
// Add / Remove
// ---------------------------------------------------------------------------

core::Result<std::uint32_t> InventorySystem::Add(PlayerId player, ItemId def_id,
                                                 std::uint32_t count, core::TraceID) noexcept {
    const ItemDef* def = store_.Lookup(def_id);
    if (def == nullptr) return core::Result<std::uint32_t>::Fail(NotFound("def_id not in store"));
    if (count == 0) return core::Result<std::uint32_t>::Fail(BadArg("count==0"));

    Inventory& inv = inv_[player];  // 惰性创建该玩家背包
    auto r = inv.Add(*def, count);
    if (!r.HasValue()) return core::Result<std::uint32_t>::Fail(r.Err());
    Emit(ItemAdded{r.Value().guid, def_id, r.Value().added, /*source=*/0});
    return core::Result<std::uint32_t>::Ok(r.Value().added);
}

core::Result<std::uint32_t> InventorySystem::Remove(PlayerId player, ItemGuid guid,
                                                    std::uint32_t count, core::TraceID) noexcept {
    auto it = inv_.find(player);
    if (it == inv_.end()) return core::Result<std::uint32_t>::Fail(BadArg("player has no inventory"));
    Inventory& inv = it->second;

    const SlotIndex i = inv.Find(guid);
    if (i == kMaxInventorySlots) return core::Result<std::uint32_t>::Fail(NotFound("guid not in bag"));
    const ItemStack* st = inv.At(i);
    if (st == nullptr) return core::Result<std::uint32_t>::Fail(NotFound("guid not in bag"));
    const ItemId def_id = st->def_id;

    auto r = inv.Remove(guid, count);
    if (!r.HasValue()) return core::Result<std::uint32_t>::Fail(r.Err());
    Emit(ItemRemoved{guid, def_id, r.Value(), /*reason=*/0});
    return core::Result<std::uint32_t>::Ok(r.Value());
}

// ---------------------------------------------------------------------------
// Equip / Unequip
// ---------------------------------------------------------------------------

core::Result<void> InventorySystem::Equip(PlayerId player, ItemGuid guid, EquipSlot slot,
                                           core::TraceID) noexcept {
    if (slot == EquipSlot::Count) return core::Result<void>::Fail(BadArg("invalid slot"));
    auto it = inv_.find(player);
    if (it == inv_.end()) return core::Result<void>::Fail(BadArg("player has no inventory"));
    Inventory& inv = it->second;

    const SlotIndex i = inv.Find(guid);
    if (i == kMaxInventorySlots) return core::Result<void>::Fail(BadArg("guid not in bag"));
    const ItemStack* stp = inv.At(i);
    if (stp == nullptr) return core::Result<void>::Fail(BadArg("guid not in bag"));
    const ItemStack st = *stp;  // 复制（Remove 会清空槽）
    const ItemDef* def = store_.Lookup(st.def_id);
    if (def == nullptr) return core::Result<void>::Fail(NotFound("def_id not in store"));

    // 槽位匹配校验
    if ((EquipBit(slot) & def->equip_slots) == 0) {
        return core::Result<void>::Fail(BadArg("slot mismatch"));
    }
    // 等级校验（经 RoleSystem 公开接口取角色）
    role::Character* c = role_.FindByPlayer(player);
    if (c == nullptr) return core::Result<void>::Fail(BadArg("no character for player"));
    if (c->level < def->required_level) {
        return core::Result<void>::Fail(BadArg("level insufficient"));
    }
    // 同槽已占用 → 拒绝（防覆盖）
    if (inv.Equipped(slot) != 0) {
        return core::Result<void>::Fail(BadArg("slot occupied"));
    }

    // 从背包移除（装备 max_stack=1，count=1 必成功）
    auto rm = inv.Remove(guid, 1);
    if (!rm.HasValue()) return core::Result<void>::Fail(rm.Err());

    // 放入装备槽（保留 guid + durability）
    inv.SetEquipped(slot, st);

    // 重算 from_equipment 层并触发 Role::RecomputeAttributes（§21 禁改 Final）
    RecomputeEquipmentAttrs(player);

    const std::int64_t delta = EffectiveContributionL1(*def, st.durability);
    Emit(ItemEquipped{guid, slot, delta});
    return core::Result<void>::Ok();
}

core::Result<void> InventorySystem::Unequip(PlayerId player, EquipSlot slot, core::TraceID) noexcept {
    if (slot == EquipSlot::Count) return core::Result<void>::Fail(BadArg("invalid slot"));
    auto it = inv_.find(player);
    if (it == inv_.end()) return core::Result<void>::Fail(BadArg("player has no inventory"));
    Inventory& inv = it->second;

    const ItemGuid g = inv.Equipped(slot);
    if (g == 0) return core::Result<void>::Fail(BadArg("slot empty"));
    const ItemStack eq = inv.EquippedStack(slot);  // 复制
    const ItemDef* def = store_.Lookup(eq.def_id);
    if (def == nullptr) return core::Result<void>::Fail(NotFound("def_id not in store"));

    const std::int64_t delta = -EffectiveContributionL1(*def, eq.durability);

    // 先把物品加回背包（保留身份）；背包满则拒绝且不清空装备槽（物品不丢失，§19）
    auto add = inv.AddExisting(*def, eq);
    if (!add.HasValue()) return core::Result<void>::Fail(add.Err());

    inv.ClearEquipped(slot);
    RecomputeEquipmentAttrs(player);
    Emit(ItemUnequipped{g, slot, delta});
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// 耐久
// ---------------------------------------------------------------------------

core::Result<void> InventorySystem::DamageDurability(PlayerId player, EquipSlot slot,
                                                     std::uint32_t amount, core::TraceID) noexcept {
    if (slot == EquipSlot::Count) return core::Result<void>::Fail(BadArg("invalid slot"));
    auto it = inv_.find(player);
    if (it == inv_.end()) return core::Result<void>::Fail(BadArg("player has no inventory"));
    Inventory& inv = it->second;

    const ItemGuid g = inv.Equipped(slot);
    if (g == 0) return core::Result<void>::Fail(BadArg("slot empty"));
    ItemStack& eq = inv.EquippedStack(slot);
    const ItemDef* def = store_.Lookup(eq.def_id);
    if (def == nullptr) return core::Result<void>::Fail(NotFound("def_id not in store"));

    const std::uint32_t from = eq.durability;
    const std::uint32_t to = (amount >= from) ? 0u : (from - amount);
    if (to == from) return core::Result<void>::Ok();  // 无变化

    eq.durability = to;
    RecomputeEquipmentAttrs(player);  // 耐久系数变化 → 重算属性
    Emit(DurabilityChanged{g, from, to});
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// 只读视图
// ---------------------------------------------------------------------------

const Inventory* InventorySystem::View(PlayerId player) const noexcept {
    auto it = inv_.find(player);
    if (it == inv_.end()) return nullptr;
    return &it->second;
}

SlotIndex InventorySystem::UsedSlots(PlayerId player) const noexcept {
    auto it = inv_.find(player);
    if (it == inv_.end()) return 0;
    return it->second.UsedSlots();
}

// ---------------------------------------------------------------------------
// 装备属性联动（写 from_equipment 层后 Recompute）
// ---------------------------------------------------------------------------

void InventorySystem::RecomputeEquipmentAttrs(PlayerId player) noexcept {
    role::Character* c = role_.FindByPlayer(player);
    if (c == nullptr) return;  // 无角色则无法联动（调用方保证 Scene 内有角色）

    // 清零 from_equipment 层，从头累加所有装备槽的有效贡献（含耐久衰减）
    std::fill(c->attrs.from_equipment.begin(), c->attrs.from_equipment.end(), 0);

    auto it = inv_.find(player);
    if (it == inv_.end()) {
        (void)role_.RecomputeAttributes(c->id);
        return;
    }
    const Inventory& inv = it->second;
    for (std::size_t s = 0; s < kEquipSlotCount; ++s) {
        const ItemGuid g = inv.Equipped(static_cast<EquipSlot>(s));
        if (g == 0) continue;
        const ItemStack& eq = inv.EquippedStack(static_cast<EquipSlot>(s));
        const ItemDef* def = store_.Lookup(eq.def_id);
        if (def == nullptr) continue;
        const int fn = DurabilityFactorN(def->max_durability, eq.durability);
        for (std::size_t i = 0; i < role::kAttrCount; ++i) {
            c->attrs.from_equipment[i] += def->attr_bonus[i] * fn / 2;
        }
    }
    (void)role_.RecomputeAttributes(c->id);
}

}  // namespace mmo::game::inventory
