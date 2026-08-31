# Inventory / Equipment — 测试（TASK-017 §16 / §17 / §19）

## 单元测试（ctest `Inventory.Suite`，`tests/inventory_test.cpp`）
1. 堆叠 / 边界：堆叠上限、超量拒绝不扣减、guid 不存在 NOT_FOUND。
2. 背包满 BUSY：100 槽填满后 Add 第 101 个 → `BUSY`，总数不变（不产生、不丢失）。
3. 装备校验：槽位不匹配（MainHand 剑放 OffHand）→ 拒绝；等级不足（req10 戒指放 1 级）→ 拒绝。
4. 属性联动：Equip 后 `Total(Attack) = before + 20`（全额耐久）；Unequip 回到 base。
5. 耐久衰减：70(>50% 满)→+20；40(<50% 减半)→+10；0 失效→+0，物品仍保留（durability=0）。
6. 事件完整性：单玩家 Add/Add/Equip/Remove/Unequip → 事件数 == 操作数、字段正确。

## 集成测试（§17）
1000 玩家 × 多操作（Add/Equip/Unequip/Damage/Remove 混合）：
- 物品守恒：全局存活数 == 全局净增减。
- 无复制：`guid` 全集排序后无重复。
- 事件数 == 操作数（每操作恰好 1 事件）。

## 失败测试（§19）
- 满包 Add → BUSY，物品不丢失（断言总数不变）。
- Remove 不存在 guid → NOT_FOUND。
- Remove 数量 > 持有 → 拒绝不扣减（防负数）。
- 重复 Equip 同 guid → 拒绝（防复制）。
- 耐久 0 仍战斗 → 属性不生效、不崩溃。

## 全局唯一性
`NewItemGuid()` 调用 100 万次：严格递增、无 0、无重复（防复制基础，TASK-030）。

## 运行
```bash
ctest -R Inventory          # 单元测试 + 集成 + 失败 + 唯一性
bin/inventory_bench --players 1000 --ops 100   # bench/inventory.txt
```
