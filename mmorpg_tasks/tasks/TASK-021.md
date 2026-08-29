---
TASK-ID: TASK-021
NAME: Skill System
PHASE: Phase 5 · 战斗
MODULE: server/gamenode/combat
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-011, TASK-016
---

# TASK-021 · Skill System

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-021` |
| NAME | Skill System |
| PHASE | Phase 5 · 战斗 |
| MODULE | `server/gamenode/combat` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-011`, `TASK-016` |

---

## 1. Objective

实现技能系统第一版：SkillDefinition / Cast / Cooldown / Cost / Range / Target / Effect。先支持 Single Target / AOE / Self / Projectile 四类，不追求复杂。

## 2. Dependencies

### 2.1 前置任务

- `TASK-011` · Entity System
- `TASK-016` · Player / Character

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 011 016`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/combat`

## 4. State Owner（状态归属）

技能配置（定义表）不可变，归配置系统；技能运行时状态（CD、施法进度、层数、充能）的 Owner 是 Combat System，只在 Scene 线程内写。Skill 不得直接读 Role 私有成员，只能经 Role Command / Interface 访问。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-011 Entity/Position；TASK-016 属性（技能消耗与效果计算基础）

## 6. Output

skill 模块（定义/施法/冷却/消耗/目标）+ 四类技能测试

## 7. Public Interface

```cpp
namespace mmo::game::combat {
using SkillId = uint32_t;
enum class TargetType : uint8_t { Self, SingleTarget, AoeCircle, AoeCone, Projectile };
enum class CastResult : uint8_t { Ok, OnCooldown, OutOfRange, NoTarget, InsufficientResource,
                                  Interrupted, InvalidTarget, Silenced };
struct SkillDef { SkillId id; std::string_view name; TargetType target_type;
                  float cast_time{0.0f};       // 0 = 瞬发
                  float cooldown{1.5f}; float range{5.0f}; float radius{0.0f};
                  int64_t mana_cost{0}; int64_t hp_cost{0};
                  std::vector<EffectDef> effects;   // 效果列表（伤害/治疗/buff）
                  bool interruptible{true}; uint32_t required_level{1}; };  // 配置化
struct CastRequest { core::RequestID request_id; entity::EntityId caster; SkillId skill;
                     entity::EntityId target; Position target_pos; core::TraceID trace; };
class SkillSystem { public:
  core::Result<CastResult> TryCast(const CastRequest&, const scene::SceneContext&);
  core::Result<void> Update(const scene::SceneContext&);    // 处理读条完成与发射物
  core::Result<void> InterruptCasting(entity::EntityId, InterruptReason, core::TraceID);
  bool IsOnCooldown(entity::EntityId, SkillId) const noexcept;
  DurationMs CooldownRemaining(entity::EntityId, SkillId) const noexcept;
  CastingState CastingOf(entity::EntityId) const noexcept; };
}
```

## 8. Data Model

**施法状态机**：`Idle → Casting(读条) → Resolved(结算) → Cooldown → Idle`

**效果（EffectDef，第一版四类）**

| 类型 | 参数 | 说明 |
|---|---|---|
| Damage | school, base, coeff, can_crit | 产生 DamageEvent（TASK-022） |
| Heal | base, coeff | 产生 HealEvent |
| ApplyBuff | buff_id, duration, stacks | 交给 TASK-023 |
| SpawnProjectile | speed, radius, max_distance | 飞行物实体，接触后结算 |

**目标选取**：SingleTarget 校验距离与敌对；AOE 用 AOI 查询范围内实体（局部查询，禁止全扫）；Self 无视距离。

## 9. Thread Model

技能在 Scene 的 SimulationThread 的 Combat 阶段执行。读条计时走 TASK-004 Scheduler（同线程 Tick 驱动），禁止另起线程。

## 10. Hot Path

**YES**

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- server/gamenode/combat/include/mmo/game/combat/skill/
- server/gamenode/combat/src/skill/
- server/gamenode/combat/tests/
- config/gameplay/skills/

## 15. Implementation Steps

1. 定义 skill_def.h：SkillDef / EffectDef / TargetType，全部配置化（config/gameplay/skills/*.json）
2. 实现 skill_registry.h：技能表加载与校验（引用不存在的 buff → 加载失败报错）
3. 实现 cooldown_tracker.h：每实体每技能冷却表（用时间戳数组，O(1) 查询，禁止 map 查找热路径）
4. 实现 skill_system.h/.cpp：TryCast（校验 → 扣资源 → 读条或瞬发 → 结算）
5. 实现四类目标：Self（自身）、SingleTarget（距离+敌对校验）、AoeCircle/AoeCone（AOI 范围查询）、Projectile（生成飞行物实体）
6. 实现读条与打断：cast_time > 0 进 Casting 状态，可被打断（受击/控制），打断发布 SkillInterrupted 事件
7. 实现资源消耗：mana_cost / hp_cost 在施法开始时扣除，打断**不退还**（写死并测试）
8. 实现飞行物：Projectile 实体按速度移动，接触目标或达最大距离后结算（移动走 MovementSystem）
9. 写测试：四类技能各自的正常与异常路径；冷却计时；资源不足；距离不足；读条打断；打断不退资源；飞行物命中与超距消失
10. 写配置：config/gameplay/skills/*.json 至少 12 个技能（覆盖四类目标 + 四种效果）

## 16. Unit Test

四类技能 TryCast 全路径与 CastResult 各枚举；冷却计时与查询；资源扣除与不退还；距离校验；读条状态机；打断；飞行物生命周期；配置加载校验

## 17. Integration Test

一个 5 人小队打 20 只怪：混合使用四类技能 1000 次，断言事件序列正确（SkillCast → DamageEvent → 目标掉血）、冷却生效（同一技能在冷却内被拒）、资源正确扣除、无资源凭空产生

## 18. Benchmark

bin/skill_bench：`try_cast_ns=` / `cooldown_query_ns=` / `aoe_target_select_ns=` / `mem_bytes_per_skill_state=`

## 19. Failure Test

技能引用了不存在的 buff：加载期报错，禁止运行期才崩；目标在施法过程中消失：瞬发技能校验失败返回 NoTarget，读条技能打断；冷却边界（恰好 = cooldown 时刻）：允许施放（测试明确定义）；飞行物目标中途死亡：飞行物继续飞行后消失，不崩溃；并发施放同一技能（客户端重复发包）：第二次因冷却被拒（防重复结算）

## 20. Acceptance Criteria

1. 四类目标（Self / SingleTarget / AOE / Projectile）全部实现且有测试
2. 冷却、消耗、距离、目标四类校验齐全，CastResult 各分支可达
3. 读条可被中断且中断不退资源（单测断言）
4. AOE 目标选取走 AOI 局部查询（grep + benchmark 佐证）
5. 客户端重复发包不会重复结算（冷却拦截，集成测试）
6. 技能配置全部配置化（代码无硬编码数值）
7. Debug / Release 双构建通过，ctest -R Skill 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止用全 Scene 扫描做 AOE 目标选取（走 AOI）
- 禁止为读条另起线程或每技能一个定时器
- 禁止硬编码技能数值
- 禁止打断时退还已扣资源（第一版策略，如需改需 RFC）
- 禁止允许冷却边界抖动导致可重复施放
- 禁止在技能结算中做数据库/网络访问

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

TryCast < 1us；冷却查询 < 20ns；AOE（半径 8m，平均 20 目标）目标选取 < 10us；单实体技能状态内存 < 256B。

## 23. Deliverables

- server/gamenode/combat/include/mmo/game/combat/skill/skill_def.h
- server/gamenode/combat/include/mmo/game/combat/skill/skill_system.h
- server/gamenode/combat/src/skill/*.cpp
- server/gamenode/combat/tests/*
- config/gameplay/skills/*.json
- server/gamenode/combat/docs/INTERFACE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-021.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-021.sh
bash scripts/verify/task-021.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 011 016`
2. 交付物存在性检查（1 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Skill`
5. Benchmark 执行：`bin/skill_bench --casts 100000`
6. 性能阈值断言：`bench/skill.txt` 中 `try_cast_ns` ≤ `1000`
7. 性能阈值断言：`bench/skill.txt` 中 `cooldown_query_ns` ≤ `20`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-021

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Skill System

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-021.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-021
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
3. 查依赖：确认 TASK-011, TASK-016 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-021.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-011` · `server/gamenode/entity`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
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
