---
TASK-ID: TASK-019
NAME: Quest System
PHASE: Phase 4 · 基础 MMORPG
MODULE: server/gamenode/quest
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-007, TASK-016, TASK-018
---

# TASK-019 · Quest System

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-019` |
| NAME | Quest System |
| PHASE | Phase 4 · 基础 MMORPG |
| MODULE | `server/gamenode/quest` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-007`, `TASK-016`, `TASK-018` |

---

## 1. Objective

实现事件驱动的任务系统：QuestDefinition / QuestInstance / Objective / Progress / Reward。**禁止每秒遍历所有玩家检查所有任务。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-007` · Command / Query / Event Bus
- `TASK-016` · Player / Character
- `TASK-018` · NPC / Monster / AI

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 007 016 018`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/quest`

## 4. State Owner（状态归属）

任务进度（QuestProgress）的 Owner 是 Quest System，只在 Scene 线程内按事件驱动更新，禁止其他模块直接写进度。事件索引（事件类型 → 关心该事件的任务集合）由 Quest 独占维护并常驻内存，用于彻底避免「每秒遍历全服玩家检查所有任务」的反模式。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-007 EventBus（事件驱动核心）；TASK-016 Role（奖励发放）；TASK-018 击杀事件源

## 6. Output

quest 模块（事件驱动进度）+ 任务链测试 + 反模式验证（禁止轮询）

## 7. Public Interface

```cpp
namespace mmo::game::quest {
using QuestId = uint32_t;
enum class ObjectiveType : uint8_t { KillMonster, CollectItem, TalkNpc, ReachLocation, UseItem };
struct ObjectiveDef { ObjectiveType type; uint32_t target_id; uint32_t required_count; };
struct QuestDef { QuestId id; std::string_view title; uint32_t required_level;
                  std::vector<QuestId> prerequisites; std::vector<ObjectiveDef> objectives;
                  uint64_t exp_reward; uint64_t currency_reward; std::vector<ItemId> item_rewards; };
struct QuestInstance { QuestId def_id; uint32_t version{0};
                       std::vector<uint32_t> progress;   // 与 objectives 下标对应
                       QuestStatus status; core::SteadyTime accepted_at; };
enum class QuestStatus : uint8_t { Accepted, Completed, TurnedIn, Failed, Abandoned };
class QuestSystem { public:
  core::Result<void> Accept(PlayerId, QuestId, core::TraceID);
  core::Result<void> Abandon(PlayerId, QuestId, core::TraceID);
  core::Result<void> TurnIn(PlayerId, QuestId, core::TraceID);
  core::Result<void> OnEvent(const core::EventEnvelope&);   // 事件驱动入口
  const QuestInstance* Find(PlayerId, QuestId) const noexcept;
  size_t ActiveQuests(PlayerId) const noexcept;
  size_t EventHandlerCount() const noexcept;   // 指标：注册的事件处理器数
};
}
```

## 8. Data Model

**事件 → 进度映射（订阅表，第一版四类事件）**

| 事件 | 匹配目标 | 进度更新 |
|---|---|---|
| MonsterKilled(npc_def_id, killer) | ObjectiveType::KillMonster && target_id == npc_def_id | progress[i] += 1 |
| ItemAdded(item_def_id, player) | CollectItem && target_id == item_def_id | progress[i] += count |
| NpcTalked(npc_def_id, player) | TalkNpc && target_id == npc_def_id | progress[i] = required |
| LocationReached(zone_id, player) | ReachLocation && target_id == zone_id | progress[i] = required |

**反模式红线**：禁止注册一个「每 Tick 遍历所有玩家所有任务」的处理器。进度更新只由事件触发。
**索引**：玩家 → 进行中任务 → 按 ObjectiveType 建索引，事件到达时只查该类型相关的玩家任务集合。

## 9. Thread Model

Quest 事件处理在 Scene 的 Quest 阶段（EventBus Drain 之后）执行。任务数据由 SimulationThread 单 Owner。奖励发放走 EconomyCommand（TASK-029 未就绪时用幂等占位接口）。

## 10. Hot Path

**NO** （事件驱动，非每 Tick 全量）


## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**YES** （异步存档）

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/gamenode/quest/include/mmo/game/quest/
- server/gamenode/quest/src/
- server/gamenode/quest/tests/
- config/gameplay/quests/

## 15. Implementation Steps

1. 定义 quest_def.h：QuestDef / ObjectiveDef / QuestStatus，全部配置化（config/gameplay/quests/*.json）
2. 定义 quest_instance.h：QuestInstance 与进度结构
3. 实现 quest_index.h：按 ObjectiveType + target_id 建倒排索引（玩家任务 → 索引），事件到达 O(1) 定位
4. 实现 quest_system.h/.cpp：Accept / Abandon / TurnIn / OnEvent
5. 实现前置校验：等级不足、前置任务未完成 → 拒绝接受（返回明确错误码）
6. 实现完成检测：所有 objective 达标 → status=Completed + 发布 QuestCompleted 事件
7. 实现奖励发放：TurnIn 时调用 Economy 接口（幂等 key = player+quest，防重复领取）
8. 实现任务链：prerequisites 字段支持链式任务，前置完成后才可选
9. 实现放弃与失败：Abandon 清除进度；限时任务超时 → Failed（走 Scheduler）
10. 写测试：四类事件的进度更新；任务链前置校验；重复交任务（幂等，只发一次奖励）；放弃后重新接受；配置化加载
11. 写**反模式验证测试**：注入 1000 玩家 × 10 任务，触发 1 万次事件，断言 Quest 系统处理耗时与玩家总数**无关**（只与相关任务数相关）——若耗时随玩家数线性增长则判定失败

## 16. Unit Test

Accept/Abandon/TurnIn 与前置校验；四类事件进度更新；倒排索引命中正确性；完成检测；幂等交任务；配置加载与校验（缺字段报错）

## 17. Integration Test

1000 玩家各持 10 个任务，事件流 1 万次（击杀/拾取/对话/到达混合）：无玩家遍历（性能断言）、进度正确、奖励只发一次；任务链：完成任务 A 后任务 B 变为可选

## 18. Benchmark

bin/quest_bench：`event_handle_ns=` / `quest_update_per_1k_events_us=` / `mem_bytes_per_quest=` / `scaling_check_1k_vs_10k_players=`

## 19. Failure Test

重复交任务（客户端重发）：幂等，奖励只发一次（TASK-030 未就绪时用本地幂等表，接口一致）；任务配置引用了不存在的物品：加载时报错，禁止静默发放空气；玩家下线后事件到达：忽略或缓存（写死一种并测试）；奖励发放失败：任务不标记 TurnedIn，可重试；事件风暴（1 秒 10 万事件）：倒排索引定位不受影响，超时任务进下帧

## 20. Acceptance Criteria

1. 四类事件（MonsterKilled/ItemCollected/NPCTalked/LocationReached）驱动进度全部实现
2. **不存在每 Tick 遍历所有玩家的处理器**（反模式测试：耗时与玩家总数无关）
3. 倒排索引：事件到达 O(1) 定位相关任务（benchmark 佐证）
4. 交任务幂等，重复请求只发一次奖励（单测断言）
5. 任务链前置校验生效
6. 任务定义全部配置化（代码无硬编码）
7. Debug / Release 双构建通过，ctest -R Quest 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止每秒/每 Tick 遍历所有玩家检查所有任务
- 禁止在任务系统中做数据库同步访问
- 禁止硬编码任务配置
- 禁止重复发放任务奖励（必须幂等）
- 禁止事件处理器做阻塞 IO
- 禁止任务进度更新依赖轮询

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单次事件处理 < 1us；1 万事件处理耗时与玩家总数无关（scaling check 比值 < 1.5）；单任务实例内存 < 128B；任务配置加载（1000 条）< 50ms。

## 23. Deliverables

- server/gamenode/quest/include/mmo/game/quest/quest_def.h
- server/gamenode/quest/include/mmo/game/quest/quest_system.h
- server/gamenode/quest/include/mmo/game/quest/quest_index.h
- server/gamenode/quest/src/*.cpp
- server/gamenode/quest/tests/*
- config/gameplay/quests/*.json
- server/gamenode/quest/docs/INTERFACE.md
- server/gamenode/quest/docs/README.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-019.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-019.sh
bash scripts/verify/task-019.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 007 016 018`
2. 交付物存在性检查（1 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Quest`
5. Benchmark 执行：`bin/quest_bench --players 1000 --events 10000`
6. 性能阈值断言：`bench/quest.txt` 中 `event_handle_ns` ≤ `1000`
7. 性能阈值断言：`bench/quest.txt` 中 `scaling_check_1k_vs_10k_players` ≤ `1.5`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-019

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Quest System

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-019.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-019
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
3. 查依赖：确认 TASK-007, TASK-016, TASK-018 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-019.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-007` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-016` · `server/gamenode/role`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
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
