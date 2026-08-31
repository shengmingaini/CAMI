# Inventory / Equipment — 接口契约（TASK-017）

冻结日期：STATUS: DONE 后本文件即契约。破坏性变更须走 `version` + 兼容性评估。

## 命名空间
`mmo::game::inventory`（与 aoi / scheduler / movement / role 同级，非 `mmo::game::scene`）。

## 公开类型
- `ItemId` / `ItemGuid` / `SlotIndex`：uint64 / uint64 / uint16。
- `EquipSlot`：Head, Chest, Hands, Legs, Feet, MainHand, OffHand, Ring1, Ring2, Neck（≤10 槽）。
- `ItemDef`：静态只读配置（含 `attr_bonus` 11 维，按 `AttrType` 下标）。`name` 为 `string_view`，生命周期由 `ItemDefStore` 持有。
- `ItemStack`：实例（24B，§22 < 64B）：`def_id / count / durability / guid`。
- `ItemGuid`：全局唯一、单调递增、不可复用（防复制，TASK-030 基础）。`0` 保留为无效。

## InventorySystem（§7 冻结签名）
```cpp
explicit InventorySystem(const ItemDefStore&, role::RoleSystem&) noexcept;
void BindEventBus(core::EventBus&) noexcept;
core::Result<uint32_t> Add(PlayerId, ItemId def_id, uint32_t count, core::TraceID) noexcept;
core::Result<uint32_t> Remove(PlayerId, ItemGuid, uint32_t count, core::TraceID) noexcept;
core::Result<void>     Equip(PlayerId, ItemGuid, EquipSlot, core::TraceID) noexcept;
core::Result<void>     Unequip(PlayerId, EquipSlot, core::TraceID) noexcept;
core::Result<void>     DamageDurability(PlayerId, EquipSlot, uint32_t amount, core::TraceID) noexcept;
const Inventory*       View(PlayerId) const noexcept;     // 只读，禁止返回可变引用
SlotIndex              UsedSlots(PlayerId) const noexcept;
std::size_t            PlayerCount() const noexcept;
```

## 命令 / 事件（§8，缺失即为 bug）
| 操作 | Command | Event |
|---|---|---|
| Add | AddItemCommand | ItemAdded{guid, def_id, count, source} |
| Remove | RemoveItemCommand | ItemRemoved{guid, def_id, count, reason} |
| Equip | EquipCommand | ItemEquipped{guid, slot, attr_delta} |
| Unequip | UnequipCommand | ItemUnequipped{guid, slot, attr_delta} |
| 耐久 | — | DurabilityChanged{guid, from, to} |

事件值类型 ≤32B、nothrow move、alignof≤8。`attr_delta` 为该装备有效属性贡献的 L1 范数（Equip 正 / Unequip 负），因事件 ≤32B 约束下无法内联 11 维 int64 数组，故用标量（可观测、可断言）。

## 属性联动（§21 禁改 Final）
Equip/Unequip/DamageDurability 通过 `RoleSystem::RecomputeAttributes` 重算；装备贡献写入 `Character::attrs.from_equipment` 层（仅本层，不动 base/buff），由 `AttributeSet::Recompute()` 生成 Final。耐久衰减系数：无耐久装备全额；<50% 减半；=0 失效（属性不生效，物品保留）。

## 红线
- 背包操作只允许经 `InventorySystem`（Command/Event），禁止外部直改容器。
- 背包满 → `BUSY`，物品不产生、不丢失；Remove 超量拒绝不扣减；重复 Equip 同 guid 拒绝（防复制）。
- 不依赖 `core::ConfigManager` 全局快照；物品表由 `ItemDefStore` 受限 JSON 解析加载（多文件同键冲突）。
- 禁止 `#include` 内部 `src/`；禁止访问 role 模块内部数据（仅经公开接口）。
