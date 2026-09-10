---
TASK-ID: TASK-017
NAME: Inventory / Equipment
PHASE: Phase 4 · 基础 MMORPG
MODULE: server/gamenode/inventory
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-007, TASK-016
---

# TASK-017 · Inventory / Equipment

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-017` |
| NAME | Inventory / Equipment |
| PHASE | Phase 4 · 基础 MMORPG |
| MODULE | `server/gamenode/inventory` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-007`, `TASK-016` |

---

## 1. Objective

实现 Item / ItemStack / Inventory / Equipment / EquipmentSlot / Durability。**所有修改（Add / Remove / Equip / Unequip）必须产生标准 Command 与 Event。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-007` · Command / Query / Event Bus
- `TASK-016` · Player / Character

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 007 016`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/inventory`

## 4. State Owner（状态归属）

背包与装备栏的权威 Owner 是 Inventory System（Scene 线程内）。物品实例 ID 全局唯一、不可复用。任何增删必须生成 Command 并留痕，禁止直接改容器。装备变更触发的属性重算只能在 Scene 线程内完成，禁止异步改属性。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-007 CommandBus/EventBus；TASK-016 AttributeSet（装备影响属性）

## 6. Output

inventory 模块 + 装备属性联动测试 + 事件完整性测试

## 7. Public Interface

```cpp
namespace mmo::game::inventory {
using ItemId = uint64_t; using SlotIndex = uint16_t;
struct ItemDef { ItemId def_id; std::string_view name; uint32_t max_stack{1};
                 uint8_t item_type; uint16_t required_level; uint32_t max_durability{0};
                 std::array<int64_t, kAttrCount> attr_bonus; };   // 静态配置，只读
struct ItemStack { ItemId def_id; uint32_t count{1}; uint32_t durability{0};
                   ItemGuid guid{0}; };                            // guid 唯一，防复制
enum class EquipSlot : uint8_t { Head, Chest, Hands, Legs, Feet, MainHand, OffHand, Ring1, Ring2, Neck };
struct AddItemCommand    { core::RequestID request_id; PlayerId player; ItemId def_id; uint32_t count; core::TraceID trace; };
struct RemoveItemCommand { core::RequestID request_id; PlayerId player; ItemGuid guid; uint32_t count; core::TraceID trace; };
struct EquipCommand      { core::RequestID request_id; PlayerId player; ItemGuid guid; EquipSlot slot; core::TraceID trace; };
class InventorySystem { public:
  core::Result<uint32_t> Add(PlayerId, ItemId def_id, uint32_t count, core::TraceID);
  core::Result<uint32_t> Remove(PlayerId, ItemGuid, uint32_t count, core::TraceID);
  core::Result<void> Equip(PlayerId, ItemGuid, EquipSlot, core::TraceID);
  core::Result<void> Unequip(PlayerId, EquipSlot, core::TraceID);
  core::Result<void> DamageDurability(PlayerId, EquipSlot, uint32_t amount, core::TraceID);
  const Inventory* View(PlayerId) const noexcept;    // 只读视图，禁止返回可变引用
  size_t UsedSlots(PlayerId) const noexcept; };
}
```

## 8. Data Model

**事件（必须全部产生）**

| 操作 | Command | Event |
|---|---|---|
| Add | AddItemCommand | ItemAdded(item_guid, def_id, count, source) |
| Remove | RemoveItemCommand | ItemRemoved(item_guid, def_id, count, reason) |
| Equip | EquipCommand | ItemEquipped(item_guid, slot, attr_delta) |
| Unequip | UnequipCommand | ItemUnequipped(item_guid, slot, attr_delta) |
| 耐久 | （内部） | DurabilityChanged(item_guid, from, to) |

**ItemGuid**：全局唯一（服务器生成），**装备类物品强制唯一**，是 TASK-030 防复制的基础。
**堆叠规则**：`max_stack > 1` 才可堆叠，装备类固定 max_stack=1。

## 9. Thread Model

背包操作由 SimulationThread 执行（单 Owner）。所有修改通过 CommandBus 进入，禁止外部直接改内部结构。事件在 Tick 的 Event 阶段派发。

## 10. Hot Path

**NO** （但 Add/Remove 频繁，需保证 O(1)~O(k)）


## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**YES** （异步存档）

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/gamenode/inventory/include/mmo/game/inventory/
- server/gamenode/inventory/src/
- server/gamenode/inventory/tests/
- server/gamenode/inventory/docs/
- config/gameplay/items/

## 15. Implementation Steps

1. 定义 item.h：ItemDef（静态配置只读）/ ItemStack / ItemGuid 生成器（服务器唯一）
2. 定义 inventory.h：背包容器（定长槽位数组 + 空闲槽位栈），禁止无界
3. 定义 commands.h：Add/Remove/Equip/Unequip 四个 Command 与对应 Event
4. 实现 inventory_system.h/.cpp：Add（堆叠/占空槽）、Remove（校验数量与 guid）、Equip（槽位校验 + 等级校验）、Unequip
5. 实现装备属性联动：Equip/Unequip 后调用 RoleSystem::RecomputeAttributes，并写入 from_equipment 层
6. 实现耐久：DamageDurability 触发属性衰减（耐久 < 50% 属性减半，可配），耐久归零则装备失效（属性不生效但物品保留）
7. 实现容量限制：背包满返回 BUSY；槽位占用 O(1)（空闲栈）
8. 实现事件发布：四种操作全部产生 Event，**事件缺失即为 bug**（用测试断言事件数）
9. 写测试：堆叠/拆分、满包、guid 唯一性、装备等级不足、槽位不匹配（单手杖放副手）、耐久衰减与归零、事件完整性
10. 写配置：config/gameplay/items/*.json 至少 20 件物品（含武器/防具/消耗品）用于测试

## 16. Unit Test

Add/Remove 数量正确与边界（0、超量、堆叠上限）；Equip/Unequip 槽位与等级校验；guid 唯一（100 万次无重复）；耐久衰减与归零；事件产生完整性与字段正确；容量上限

## 17. Integration Test

1000 个玩家各执行 100 次随机背包操作（Add/Remove/Equip/Unequip 混合），断言：物品守恒（总数与操作流水一致）、无复制（guid 集合无重复）、属性联动正确（装备后 Total 值符合预期）、事件数量 = 操作数量

## 18. Benchmark

bin/inventory_bench：`add_ns=` / `remove_ns=` / `equip_ns=` / `recompute_after_equip_ns=` / `mem_bytes_per_item=`

## 19. Failure Test

背包满时 Add：返回 BUSY，物品**不产生、不丢失**（断言总数不变）；Remove 不存在的 guid：返回 NOT_FOUND；Remove 数量大于持有：拒绝，不扣减（防负数）；重复 Equip 同一 guid：拒绝（防复制）；并发 Equip 同一槽位：串行化，第二个失败而非覆盖；耐久为 0 时继续战斗：装备属性不生效，不崩溃

## 20. Acceptance Criteria

1. **四种操作全部产生标准 Command + Event**（集成测试断言事件数 == 操作数）
2. 装备类物品 ItemGuid 全局唯一（100 万次无重复，防复制）
3. 装备/卸下正确触发属性重算（from_equipment 层）
4. 背包满、物品不足、等级不足、槽位不匹配等边界全部有测试且行为明确
5. 无物品复制、无物品丢失（1000 玩家 × 100 次随机操作守恒校验）
6. 配置化物品表，代码无硬编码物品属性
7. Debug / Release 双构建通过，ctest -R Inventory 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止外部直接修改背包内部结构（必须走 Command）
- 禁止产生无 guid 的装备物品
- 禁止物品凭空产生或丢失（所有变更必须有 Command 与 Event）
- 禁止背包无界增长
- 禁止直接改 Final 属性（必须写 from_equipment 层后 Recompute）
- 禁止在背包操作中做同步数据库访问

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Add < 200ns；Remove < 200ns；Equip（含属性重算）< 2us；单物品内存 < 64B；单玩家背包内存 < 8KB（100 槽）。

## 23. Deliverables

- server/gamenode/inventory/include/mmo/game/inventory/item.h
- server/gamenode/inventory/include/mmo/game/inventory/inventory.h
- server/gamenode/inventory/include/mmo/game/inventory/inventory_system.h
- server/gamenode/inventory/src/*.cpp
- server/gamenode/inventory/tests/*
- config/gameplay/items/*.json
- server/gamenode/inventory/docs/INTERFACE.md
- server/gamenode/inventory/docs/README.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-017.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-017.sh
bash scripts/verify/task-017.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 007 016`
2. 交付物存在性检查（5 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Inventory`
5. Benchmark 执行：`bin/inventory_bench --players 1000 --ops 100`
6. 性能阈值断言：`bench/inventory.txt` 中 `equip_ns` ≤ `2000`
7. 性能阈值断言：`bench/inventory.txt` 中 `mem_bytes_per_item` ≤ `64`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-017

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Inventory / Equipment

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-017.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-017
EOF

# 3) 推送：GFW 屏蔽 ssh.github.com，必须走 22 端口
git push git@github.com:22:shengmingaini/CAMI.git main
```

提交规范：

- 类型：`feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build` / `chore`（本任务建议 `feat`）
- **一个 TASK 一次独立提交**，禁止把多个 TASK 合并提交
- 正文必须包含实测数字，禁止「性能良好 / 已优化」这类无法验证的描述
- 未通过验收脚本禁止提交，禁止 `--no-verify` 绕过

## 26. Codex Execution Rules

1. 读规范：先读 `PROJECT_REQUIREMENTS.md` 与本任务涉及章节，架构冻结，不得自行推翻。
2. 读任务：完整读完本文件全部章节再动手，禁止只看 Objective 就开始写。
3. 查依赖：确认 TASK-007, TASK-016 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-017.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-007` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-016` · `server/gamenode/role`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

### 27.3 模块边界红线（全任务统一）

- 模块 ≠ 进程：本任务代码只落在自身 `module` 子树（`include/` + `src/` + `tests/` + `docs/`），禁止扩散到其它任务拥有的目录。
- 下游只能通过本任务 `include/` 下的**公开头与接口**调用，禁止 `#include` 本任务 `src/` 或内部头（验收脚本会静态扫描本任务 `include/` 是否泄露内部 `src/`）。
- 本任务只调用依赖模块**声明**的接口，禁止访问其内部数据（如 `otherModule.internalData` 模式）。
- 接口在 `STATUS: DONE` 之后变更必须走 `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。
- 依赖方向单向（Game → Gameplay → Core），禁止循环依赖；新增模块不得破坏既有依赖环约束。

### 27.4 扩展性约束（可扩展框架兼容性）

- 新增同类能力（新 Command / 新 Event / 新 Scene 类型 / 新模块）必须走**注册表 / ID 段**机制，禁止在 `switch` 里硬编码穷举。
- 跨模块扩展点统一用抽象（C++ Interface / Command / Event），新增实现**不得修改既有任务文件**。
- 协议 / 接口变更必须带 `version` 字段并向下兼容，旧客户端 / 旧模块不得因此断连或编译失败。
- 所有模块遵循统一目录模板（include/src/tests/benchmark/docs/CMakeLists.txt）与五文档契约（README/INTERFACE/DEPENDENCY/PERFORMANCE/TEST），新增模块不得例外。

> 模块归属表（谁拥有哪棵子树）：本任务的 `module` 字段即其独占目录；
> 其它任务的 `module` 字段不得被本任务写入。统一模块模板见 `DEVELOPMENT.md` / 根规范 §6。

## 28. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-08-29 | 方案 A 原地补齐：由 `tools/gen/build_tasks.py` 从结构化数据源重新生成，补齐 State Owner / 验收脚本 / STATUS 门禁 / Git Commit 规范 |
| 2026-08-29 | 完善：新增 §27 接口契约/模块边界/扩展性（全任务统一，防相互干扰）；新增 TASK-039 Social / TASK-040 ControlService / TASK-041 集成与回归；依赖相位自检跳过最终交付汇点；Scene Migration 登记为 Phase 2 RFC |
