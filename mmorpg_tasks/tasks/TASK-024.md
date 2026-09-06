---
TASK-ID: TASK-024
NAME: Combat Framework
PHASE: Phase 5 · 战斗
MODULE: server/gamenode/combat
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DONE-DATE: 2026-09-06
DEPENDENCIES: TASK-021, TASK-022, TASK-023, TASK-018
---

# TASK-024 · Combat Framework

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-024` |
| NAME | Combat Framework |
| PHASE | Phase 5 · 战斗 |
| MODULE | `server/gamenode/combat` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-021`, `TASK-022`, `TASK-023`, `TASK-018` |

---

## 1. Objective

整合 Skill / Damage / Buff / Threat / Target，实现 CombatSystem / CombatEntity / ThreatTable / CombatState / Interrupt。**战斗热路径只能是内存、CPU 与本地接口调用。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-021` · Skill System
- `TASK-022` · Damage / Heal
- `TASK-023` · Buff / Debuff
- `TASK-018` · NPC / Monster / AI

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 021 022 023 018`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/combat`

## 4. State Owner（状态归属）

Combat Framework 是战斗流程编排者，独占战斗状态机与当前目标的写权限，但不拥有具体数值：HP 最终写入仍走 Damage / Heal，仇恨表由 Threat 独占。禁止 Combat 直接操作 Role 或 Inventory 的私有成员，一切经 Command / Interface。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-021 技能；TASK-022 伤害；TASK-023 Buff；TASK-018 AI（仇恨与战斗联动）

## 6. Output

combat 模块整合层 + 完整战斗流程测试 + 热路径性能预算验证

## 7. Public Interface

```cpp
namespace mmo::game::combat {
struct ThreatEntry { entity::EntityId source; int64_t threat; };
class ThreatTable { public:                     // 定长（默认 16），溢出按最低威胁淘汰
  void Add(entity::EntityId, int64_t amount) noexcept;
  void Remove(entity::EntityId) noexcept;
  void Scale(entity::EntityId, float factor) noexcept;   // 嘲讽/减仇
  std::optional<entity::EntityId> Top() const noexcept;  // O(n) n<=16，可接受
  void Clear() noexcept; size_t Size() const noexcept; };
enum class CombatFlag : uint32_t { None=0, InCombat=1, Casting=2, Stunned=4, Rooted=8,
                                   Silenced=16, Invulnerable=32, Dead=64 };
struct CombatEntity { entity::EntityId id; CombatComponent::Flags flags;
                      entity::EntityId target; ThreatTable threat;
                      core::SteadyTime last_combat_at; uint32_t version{0}; };
class CombatSystem { public:
  core::Result<void> EnterCombat(entity::EntityId, entity::EntityId enemy, core::TraceID);
  core::Result<void> LeaveCombat(entity::EntityId, LeaveCombatReason, core::TraceID);
  core::Result<CastResult> CastSkill(const CastRequest&, const scene::SceneContext&);
  core::Result<void> Interrupt(entity::EntityId, InterruptReason, core::TraceID);
  core::Result<void> Update(const scene::SceneContext&);    // Combat 阶段入口
  core::Result<void> OnDeath(entity::EntityId, entity::EntityId killer, core::TraceID);
  bool HasFlag(entity::EntityId, CombatFlag) const noexcept;
  CombatStats Stats() const noexcept;   // 战斗中的实体数、每秒结算次数、阶段耗时
};
}
```

## 8. Data Model

**仇恨规则（第一版）**

| 来源 | 仇恨值 |
|---|---|
| 造成伤害 | damage × 1.0 |
| 造成治疗 | heal × 0.5 |
| 嘲讽技能 | 强制 Top（或 threat × 1.5 + 置顶，二选一写死） |
| 距离衰减 | 超出脱战半径不清除仇恨，脱战计时触发清除 |

**脱战**：无战斗行为 6 秒（可配）后 LeaveCombat，清空仇恨。
**战斗状态位**：用位标记（CombatFlag），**禁止**用 bool 成员散落判断。

## 9. Thread Model

战斗系统在 Scene 的 SimulationThread 的 Combat 阶段执行，全部本地内存操作。禁止任何跨进程调用。

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

- server/gamenode/combat/include/mmo/game/combat/
- server/gamenode/combat/src/
- server/gamenode/combat/tests/
- server/gamenode/combat/docs/

## 15. Implementation Steps

1. 定义 combat_entity.h：CombatEntity / CombatFlag 位标记 / CombatState
2. 实现 threat_table.h/.cpp：定长 16 条，Add/Remove/Scale/Top，溢出淘汰最低威胁
3. 实现 combat_system.h/.cpp：EnterCombat / LeaveCombat / CastSkill / Interrupt / Update / OnDeath
4. 整合技能：CastSkill 委托 SkillSystem，结算后把伤害转仇恨
5. 整合伤害：ApplyDamage 委托 DamageSystem，结果转仇恨 + 触发 Buff（如受击触发）
6. 整合 Buff：控制类 Buff（Stun/Root/Silence）自动设置 CombatFlag，影响移动与施法
7. 实现打断：受击/控制时中断读条，发布 SkillInterrupted 事件
8. 实现脱战：无战斗行为 6 秒后 LeaveCombat，清仇恨、清部分 Buff（按配置）
9. 实现死亡处理：OnDeath 清 Buff、清仇恨、发布 EntityDied（供 TASK-019 任务系统消费）
10. 写测试：仇恨累积与 Top 切换；嘲讽；脱战计时；打断；控制 Buff 与移动/施法联动；死亡清理；战斗事件完整性
11. 写集成测试：5 人小队 vs 5 只精英怪的完整战斗（含坦克嘲讽、治疗、DOT、死亡），验证全流程

## 16. Unit Test

仇恨累积/移除/缩放/Top；位标记设置与清除；脱战计时；打断；控制 Buff 联动；死亡清理顺序；各子系统调用正确（用 mock 隔离）

## 17. Integration Test

完整战斗流程：进入战斗 → 坦克嘲讽拉怪 → DPS 输出 → 治疗加血 → DOT 跳伤害 → BOSS 死亡 → 脱战。断言：仇恨 Top 正确切换、伤害/治疗数值守恒、Buff 正确生效与移除、死亡后 Buff 与仇恨被清空、脱战在 6 秒后触发

## 18. Benchmark

bin/combat_bench：`combat_phase_us_at_1k=` / `cast_resolve_ns=` / `threat_update_ns=` / `alloc_per_combat_tick=`

## 19. Failure Test

仇恨表溢出（>16 个来源）：淘汰最低威胁而非崩溃或无限增长；目标死亡时正在读条：读条中断；战斗中所有参与者同时死亡：结算顺序确定（按 EntityId 排序）且只发一次事件；脱战瞬间重新进入战斗：计时重置，不产生状态错乱；CombatSystem 收到无效实体：返回 NOT_FOUND 不影响其他实体

## 20. Acceptance Criteria

1. Skill / Damage / Buff / Threat / Target 全部整合进 CombatSystem
2. 仇恨表定长 16，溢出淘汰最低威胁（单测）
3. **Combat 阶段不引用 MySQL / Redis / Kafka / 同步 gRPC**（链接符号 + 红线扫描）
4. 控制类 Buff 正确影响移动与施法（集成测试）
5. 完整战斗流程集成测试通过（含嘲讽、治疗、DOT、死亡、脱战）
6. 战斗状态用位标记（grep 无散落 bool 战斗状态判断）
7. Debug / Release 双构建通过，ctest -R Combat 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在 Combat 阶段访问 MySQL / Redis / Kafka / 同步 gRPC / 文件 IO
- 禁止仇恨表无界增长
- 禁止用散落 bool 表示战斗状态（必须位标记）
- 禁止在战斗热路径做跨进程调用
- 禁止战斗结算产生堆分配（alloc_per_combat_tick = 0）
- 禁止死亡后残留 Buff 与仇恨

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Combat 阶段（1000 实体，50% 战斗）< 1.2ms；单次技能结算 < 2us；仇恨更新 < 100ns；Tick 内战斗路径分配次数 = 0。

## 23. Deliverables

- server/gamenode/combat/include/mmo/game/combat/combat_system.h
- server/gamenode/combat/include/mmo/game/combat/threat_table.h
- server/gamenode/combat/include/mmo/game/combat/combat_entity.h
- server/gamenode/combat/src/*.cpp
- server/gamenode/combat/tests/*
- server/gamenode/combat/docs/INTERFACE.md
- server/gamenode/combat/docs/DEPENDENCY.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-024.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-024.sh
bash scripts/verify/task-024.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 021 022 023 018`
2. 交付物存在性检查（1 项）
3. 静态红线扫描：`server/gamenode/combat/src` 内禁止出现 /(mysql|redis|grpc|kafka|sql::)/
4. CMake configure + 编译（Debug + Release 双构建）
5. ctest 过滤执行：`-R Combat`
6. Benchmark 执行：`bin/combat_bench --entities 1000 --combat-ratio 0.5`
7. 性能阈值断言：`bench/combat.txt` 中 `combat_phase_us_at_1k` ≤ `1200`
8. 性能阈值断言：`bench/combat.txt` 中 `alloc_per_combat_tick` ≤ `0`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-024

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Combat Framework

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-024.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-024
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
3. 查依赖：确认 TASK-021, TASK-022, TASK-023, TASK-018 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-024.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-021` · `server/gamenode/combat`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-022` · `server/gamenode/combat`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-023` · `server/gamenode/combat`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-018` · `server/gamenode/ai`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
