---
TASK-ID: TASK-016
NAME: Player / Character
PHASE: Phase 4 · 基础 MMORPG
MODULE: server/gamenode/role
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-011, TASK-012, TASK-015
---

# TASK-016 · Player / Character

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-016` |
| NAME | Player / Character |
| PHASE | Phase 4 · 基础 MMORPG |
| MODULE | `server/gamenode/role` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-011`, `TASK-012`, `TASK-015` |

---

## 1. Objective

实现 Player 与 Character：等级、经验、HP/MP、属性、状态。**状态 Owner 是 Scene**——Role 只负责角色数据与行为状态，不负责 Scene / AOI / 网络 / MySQL。

## 2. Dependencies

### 2.1 前置任务

- `TASK-011` · Entity System
- `TASK-012` · Scene System
- `TASK-015` · Movement System

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 011 012 015`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/role`

## 4. State Owner（状态归属）

Player / Character 的属性、等级、经验由 Role System 在 Scene Simulation 线程上独占写入，持久化副本归 DataService。实时 HP / MP 的权威在 Scene / Combat，Role 只保存上限与基础属性，禁止把实时 HP 写回 Role 当作权威。Role 不负责 Scene / AOI / Network / MySQL。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-011 CombatComponent / MovementComponent；TASK-012 Scene；PROJECT_REQUIREMENTS.md 第 18 节

## 6. Output

role 模块（Player/Character/Attribute/Level）+ 属性计算测试 + 升级测试

## 7. Public Interface

```cpp
namespace mmo::game::role {
enum class AttrType : uint8_t { Strength, Agility, Intellect, Stamina,
                                MaxHp, MaxMp, Attack, Defense, CritRate, CritDamage, MoveSpeed };
struct AttributeSet {                    // 三层：base + equipment + buff，禁止互相污染
  std::array<int64_t, kAttrCount> base;
  std::array<int64_t, kAttrCount> from_equipment;
  std::array<int64_t, kAttrCount> from_buff;
  int64_t Total(AttrType) const noexcept;      // base + equipment + buff，带钳制
  void Recompute() noexcept;                   // 变更任一来源后调用，O(k) 非常数小
};
struct Character { CharacterId id; PlayerId owner; std::string name; uint32_t level{1};
                   uint64_t exp{0}; int64_t hp{0}; int64_t mp{0}; AttributeSet attrs;
                   uint32_t version{0}; };
class RoleSystem { public:
  core::Result<Character*> LoadOrCreate(PlayerId, CharacterId, const scene::SceneContext&);
  core::Result<void> AttachToScene(CharacterId, entity::EntityId avatar, scene::SceneId);
  core::Result<void> ModifyHp(CharacterId, int64_t delta, core::TraceID);   // 只改数据，战斗语义在 TASK-022
  core::Result<void> ModifyMp(CharacterId, int64_t delta, core::TraceID);
  core::Result<uint32_t> AddExp(CharacterId, uint64_t amount, core::TraceID);  // 返回新等级
  core::Result<void> RecomputeAttributes(CharacterId);
  core::Result<void> Save(CharacterId);        // 走 PersistenceAdapter，异步
  Character* Find(CharacterId) noexcept; Character* FindByPlayer(PlayerId) noexcept; };
}
```

## 8. Data Model

**属性三层模型**

```
Final = clamp( (base + equipment + buff) , min, max )
```

- `base`：等级与种族决定，升级时变更。
- `from_equipment`：TASK-017 装备系统写入（本任务预留接口）。
- `from_buff`：TASK-023 Buff 系统写入（本任务预留接口）。
- 任一层变更后调用 `Recompute()`，**禁止**各系统直接改 Final 值。

**升级曲线**（第一版）：`exp_to_next(level) = 100 * level^1.5`，配置化，禁止硬编码在代码里。

## 9. Thread Model

Role 数据由所属 Scene 的 SimulationThread 拥有并修改（单 Owner）。存档走 PersistenceAdapter 异步投递到 Persistence 线程，Tick 内不等待。

## 10. Hot Path

**YES**

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**YES** （异步存档）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO**


## 13. Persistence

**YES** （异步）

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/gamenode/role/include/mmo/game/role/
- server/gamenode/role/src/
- server/gamenode/role/tests/
- server/gamenode/role/docs/

## 15. Implementation Steps

1. 定义 attribute.h：AttrType 枚举与 AttributeSet 三层结构 + Recompute（配置化上限）
2. 定义 character.h：Character 结构（level/exp/hp/mp/attrs/version）
3. 实现 exp_curve.h：升级经验曲线，从配置读取（config/gameplay/exp_curve.json），禁止硬编码
4. 实现 role_system.h/.cpp：LoadOrCreate / AttachToScene / ModifyHp / ModifyMp / AddExp / RecomputeAttributes
5. 实现属性变更事件：AttributesChanged / LevelUp / HpChanged（HP 变更事件在 TASK-022 被伤害系统复用）
6. 实现 HP/MP 钳制：不允许超过 Max 或低于 0，边界行为写死并测试
7. 实现死亡状态：HP=0 → 设置死亡标记并发布 CharacterDied 事件（复活逻辑第一版只做「回城复活」占位）
8. 实现存档接口：Save 走 PersistenceAdapter 异步队列，失败进重试队列（为 TASK-026 预留）
9. 写测试：属性三层计算与钳制；升级曲线（1→60 级经验正确）；HP/MP 边界；AddExp 跨多级（一次给大量经验应连续升级）；死亡事件
10. 写集成测试：1000 个角色在 Scene 中加载、升级、受伤、存档，跑 1000 Tick 无异常

## 16. Unit Test

AttributeSet 三层计算与 Recompute；经验曲线配置化与边界（0 级、满级）；HP/MP 钳制与 Modify 正负；AddExp 跨级；死亡状态与事件；版本递增

## 17. Integration Test

1000 角色加载 + 随机升级/掉血 + 每 100 Tick 批量存档，跑 1000 Tick：数据一致（最终属性与逐步重算结果一致）、无泄漏、存档任务不阻塞 Tick（测量 Tick P99 无明显恶化）

## 18. Benchmark

bin/role_bench：`recompute_ns=` / `add_exp_ns=` / `modify_hp_ns=` / `mem_bytes_per_character=` / `save_enqueue_ns=`

## 19. Failure Test

存档失败（DataService 不可用）：进入重试队列并记录，Tick 不受影响；HP 修改导致负数：钳制到 0 并触发死亡事件（不产生负值）；经验溢出（uint64 接近上限）：钳制并返回错误；角色已销毁后操作：返回 NOT_FOUND；配置缺失（exp_curve.json 不存在）：加载失败返回错误，禁止用默认值静默启动

## 20. Acceptance Criteria

1. 属性三层模型（base/equipment/buff）实现，各系统不能直接改 Final 值（代码评审）
2. 经验曲线配置化，代码中无硬编码数值（grep 验证）
3. **Role 模块不访问 MySQL / Redis / 网络**（红线扫描，只走 PersistenceAdapter 接口）
4. HP/MP 边界钳制与死亡事件正确（单测）
5. 存档异步，不阻塞 Tick（集成测试测量 Tick P99）
6. 1000 角色场景内存占用达标（benchmark）
7. Debug / Release 双构建通过，ctest -R Role 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止 Role 模块直接访问 MySQL / Redis / 网络（只走 PersistenceAdapter）
- 禁止 Role 负责 Scene / AOI / 网络相关职责
- 禁止硬编码升级经验曲线（必须配置化）
- 禁止各系统直接写 Final 属性值（必须走三层来源 + Recompute）
- 禁止 HP 出现负值或超过 Max
- 禁止在 Tick 内同步等待存档完成

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单角色内存 < 512B；Recompute < 300ns；AddExp < 100ns；ModifyHp < 50ns；存档入队 < 200ns（不入 Tick 关键路径）。

## 23. Deliverables

- server/gamenode/role/include/mmo/game/role/attribute.h
- server/gamenode/role/include/mmo/game/role/character.h
- server/gamenode/role/include/mmo/game/role/role_system.h
- server/gamenode/role/src/*.cpp
- server/gamenode/role/tests/*
- config/gameplay/exp_curve.json
- server/gamenode/role/docs/INTERFACE.md
- server/gamenode/role/docs/README.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-016.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-016.sh
bash scripts/verify/task-016.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 011 012 015`
2. 交付物存在性检查（1 项）
3. 静态红线扫描：`server/gamenode/role/src` 内禁止出现 /(mysql|redis|grpc|sql::)/
4. CMake configure + 编译（Debug + Release 双构建）
5. ctest 过滤执行：`-R Role`
6. Benchmark 执行：`bin/role_bench --characters 1000`
7. 性能阈值断言：`bench/role.txt` 中 `mem_bytes_per_character` ≤ `512`
8. 性能阈值断言：`bench/role.txt` 中 `recompute_ns` ≤ `300`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-016

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Player / Character

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-016.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-016
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
3. 查依赖：确认 TASK-011, TASK-012, TASK-015 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-016.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-011` · `server/gamenode/entity`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-012` · `server/gamenode/scene`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-015` · `server/gamenode/movement`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
