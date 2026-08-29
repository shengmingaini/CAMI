---
TASK-ID: TASK-018
NAME: NPC / Monster / AI
PHASE: Phase 4 · 基础 MMORPG
MODULE: server/gamenode/ai
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-011, TASK-014, TASK-015
---

# TASK-018 · NPC / Monster / AI

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-018` |
| NAME | NPC / Monster / AI |
| PHASE | Phase 4 · 基础 MMORPG |
| MODULE | `server/gamenode/ai` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-011`, `TASK-014`, `TASK-015` |

---

## 1. Objective

实现 NPC 与 Monster 的生成/销毁，以及第一版状态机 AI：Idle / Patrol / Chase / Attack / Return / Dead。**先做状态机，不做复杂行为树。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-011` · Entity System
- `TASK-014` · AOI System（Dynamic Grid 第一版）
- `TASK-015` · Movement System

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 011 014 015`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/ai`

## 4. State Owner（状态归属）

NPC / Monster 实体的状态 Owner 是所属 Scene（与 Player 同规则）。AI 黑板由 AI 模块独占写入，但战斗相关状态（当前目标、仇恨值）由 Combat 写入，AI 只读。NPC 血量归 Scene / Combat，不归 AI。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-011 Entity/Npc/Monster；TASK-014 AOI（仇恨目标选取）；TASK-015 Movement（追击移动）

## 6. Output

ai 模块（AI 状态机 + Spawn/Despawn）+ 状态机测试 + 1000 怪长稳测试

## 7. Public Interface

```cpp
namespace mmo::game::ai {
enum class AiState : uint8_t { Idle, Patrol, Chase, Attack, Return, Dead };
constexpr const char* ToString(AiState) noexcept;
struct SpawnDef { uint32_t npc_def_id; entity::EntityType type; std::string_view name;
                  Position spawn_pos; float patrol_radius{10.0f}; float aggro_radius{15.0f};
                  float chase_leave_radius{30.0f}; uint32_t respawn_seconds{30};
                  int64_t max_hp; uint32_t level; };   // 全部配置化
struct AiComponent { AiState state{AiState::Idle}; entity::EntityId target{kInvalidEntity};
                     Position spawn_origin; core::SteadyTime state_entered_at;
                     core::SteadyTime next_decision_at; uint32_t patrol_index{0}; };
class AiSystem { public:
  core::Result<entity::EntityId> Spawn(const SpawnDef&, const scene::SceneContext&);
  core::Result<void> Despawn(entity::EntityId);
  core::Result<void> Update(const scene::SceneContext&);     // AI 阶段驱动
  core::Result<void> OnDamaged(entity::EntityId victim, entity::EntityId attacker, int64_t amount);
  AiState StateOf(entity::EntityId) const noexcept;
  size_t CountByState(AiState) const noexcept;               // 指标：状态分布
};
}
```

## 8. Data Model

**状态机转移**

```
Idle ──(巡逻计时到)──> Patrol ──(发现目标)──> Chase ──(进入攻击距离)──> Attack
  ↑                       │                     │                       │
  │                       └──(无目标)───────────┘                       │
  │                                             │(脱离追击半径)         │(目标死亡/消失)
  │                                             ▼                       │
  └─────────────────── Return ──(回到原点)───────┘<──────────────────────┘
Dead <──(HP<=0)── 任意状态；Dead ──(respawn 计时)──> Idle(重生)
```

**决策节流**：AI 决策默认每 200ms 一次（5Hz），**禁止每 Tick 全量决策**（1000 怪 × 20Hz = 2 万次/s，直接打爆预算）。

## 9. Thread Model

AI 在 Scene 的 SimulationThread 的 Quest 阶段（或独立 AI 子阶段）执行。状态由 SimulationThread 单 Owner。死亡与重生的定时走 TASK-004 Scheduler（同线程 Tick 驱动）。

## 10. Hot Path

**YES** （1000 怪 AI 是主要 CPU 消耗之一）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- server/gamenode/ai/include/mmo/game/ai/
- server/gamenode/ai/src/
- server/gamenode/ai/tests/
- server/gamenode/ai/benchmark/
- config/gameplay/npc/

## 15. Implementation Steps

1. 定义 ai_state.h：AiState 六态与转移表（显式表驱动，禁止散落 if）
2. 定义 spawn_def.h：SpawnDef 结构，全部字段来自配置（config/gameplay/npc/*.json）
3. 实现 ai_component.h：AiComponent（状态/目标/原点/计时）
4. 实现 ai_system.h/.cpp：Spawn/Despawn/Update/OnDamaged，状态转移按表驱动实现
5. 实现目标选取：用 AOI QueryVisible + 距离过滤选最近的敌对目标（只查 3×3 格，禁止全扫）
6. 实现决策节流：每个 AI 有 next_decision_at，未到时间只做轻量更新（死亡/超时检查）
7. 实现追击与返回：Chase 调用 MovementSystem 设置目标点；超出 chase_leave_radius 转 Return
8. 实现死亡与重生：Dead 状态 + Scheduler 定时重生（respawn_seconds），重生位置回到 spawn_origin
9. 实现状态分布指标：每状态的实体数（为容量分析提供依据）
10. 写测试：六态全转移路径；决策节流生效（1000 怪 1 秒内决策次数 ≈ 5000）；目标选取正确性；脱离半径；重生计时
11. 写基准：1000 个怪在 Scene 中长稳 10 分钟

## 16. Unit Test

六态转移全路径与非法转移防护；决策节流计时；目标选取（最近敌对、无视死亡目标）；脱离半径与 Return；重生计时与位置复位；Spawn/Despawn 与实体生命周期联动

## 17. Integration Test

1000 个怪（Idle/Patrol 混合）+ 50 个玩家在 Scene 中交互 10 分钟：玩家进入 aggro → 怪转 Chase → 接触转 Attack → 玩家远离 → Return → Idle；全程无状态机死锁（断言每个怪在 60 秒内至少经历一次状态评估）、无泄漏、状态分布指标合理

## 18. Benchmark

bin/ai_bench：`ai_update_ns_per_entity=` / `decisions_per_second_per_1k=` / `mem_bytes_per_ai=` / `ai_phase_us_at_1k=`

## 19. Failure Test

目标突然消失（玩家下线）：转 Return 而非卡死在 Chase；AI 决策耗时超预算：跳过本轮剩余（节流 + 预算双保险）；同时 1000 怪被拉仇恨：不产生 1000 次全量查询（用 AOI 局部查询，benchmark 佐证）；重生计时器被大量堆积：走 Scheduler 时间轮，不创建线程/不阻塞；SpawnDef 配置缺失字段：加载失败返回错误，禁止用默认值静默生成

## 20. Acceptance Criteria

1. 六态（Idle/Patrol/Chase/Attack/Return/Dead）全部实现，转移表驱动
2. **AI 决策节流生效**：1000 怪 1 秒内决策次数 ≈ 5000（5Hz），而非 20000
3. 目标选取走 AOI 局部查询，无全 Scene 扫描（grep + benchmark 佐证）
4. 死亡与重生走 Scheduler，不创建线程/不每 Buff 一个定时器
5. 1000 怪长稳 10 分钟无死锁、无泄漏、状态分布可观测
6. NPC/Monster 属性全部配置化（代码无硬编码数值）
7. Debug / Release 双构建通过，ctest -R Ai 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止每 Tick 全量 AI 决策（必须节流）
- 禁止用全 Scene 扫描选取目标（走 AOI）
- 禁止为每个怪/每个定时器创建线程
- 禁止硬编码 NPC 属性（必须配置化）
- 禁止在 AI 中做数据库或网络访问
- 禁止第一版就实现行为树（先状态机）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

1000 个 AI 实体：AI 阶段耗时 < 400us/Tick；单次决策 < 500ns；单 AI 内存 < 128B；决策频率 5Hz（可配）。

## 23. Deliverables

- server/gamenode/ai/include/mmo/game/ai/ai_state.h
- server/gamenode/ai/include/mmo/game/ai/ai_system.h
- server/gamenode/ai/include/mmo/game/ai/spawn_def.h
- server/gamenode/ai/src/*.cpp
- server/gamenode/ai/tests/*
- server/gamenode/ai/benchmark/*
- config/gameplay/npc/*.json
- server/gamenode/ai/docs/INTERFACE.md
- server/gamenode/ai/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-018.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-018.sh
bash scripts/verify/task-018.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 011 014 015`
2. 交付物存在性检查（1 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Ai`
5. Benchmark 执行：`bin/ai_bench --monsters 1000 --ticks 12000`
6. 性能阈值断言：`bench/ai.txt` 中 `ai_phase_us_at_1k` ≤ `400`
7. 性能阈值断言：`bench/ai.txt` 中 `mem_bytes_per_ai` ≤ `128`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-018

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): NPC / Monster / AI

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-018.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-018
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
3. 查依赖：确认 TASK-011, TASK-014, TASK-015 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-018.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-011` · `server/gamenode/entity`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-014` · `server/gamenode/aoi`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
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
