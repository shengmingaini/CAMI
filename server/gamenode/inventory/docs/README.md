# Inventory / Equipment（TASK-017）

背包与装备系统。物品实例全局唯一（防复制），任何增删产生标准 Command 与 Event；装备变更经
`RoleSystem` 重算属性（写 `from_equipment` 层）。

## 目录
```
server/gamenode/inventory/
  include/mmo/game/inventory/   item.h / commands.h / inventory.h / inventory_system.h
  src/                          item.cpp / inventory.cpp / inventory_system.cpp
  tests/inventory_test.cpp
  benchmark/inventory_bench.cpp
  docs/                         INTERFACE / README / DEPENDENCY / PERFORMANCE / TEST
config/gameplay/items/items.json   21 件物品（武器/防具/消耗品/药剂），配置化、无硬编码
```

## 快速使用
```cpp
ItemDefStore store; store.LoadDir("config/gameplay/items");
RoleSystem role(sink, curve);          // TASK-016
InventorySystem inv(store, role);
core::EventBus bus; inv.BindEventBus(bus);
auto a = inv.Add(player, 10 /*铁剑*/, 1, trace);   // 返回 AddOutcome{guid, added}
inv.Equip(player, a.Value().guid, EquipSlot::MainHand, trace);  // 触发属性重算 + ItemEquipped
```

## 设计要点
- 背包：100 槽定长数组 + 空闲槽栈（O(1) 占/收），禁止无界增长。
- 装备栏：独立 10 槽，存完整 `ItemStack`（保留 durability），与背包分离。
- 堆叠：`max_stack>1` 才叠；装备类固定 `max_stack=1`，独占空槽且强制唯一 guid。
- 耐久：DamageDurability 衰减；<50% 属性减半，=0 失效但物品保留。
- 事件：五类操作全部留痕，集成测试断言「事件数 == 操作数」。

## 验收
`bash scripts/verify/task-017.sh`（Debug+Release 双构建、ctest -R Inventory、bench 断言
`equip_ns≤2000` / `mem_bytes_per_item≤64`）。
