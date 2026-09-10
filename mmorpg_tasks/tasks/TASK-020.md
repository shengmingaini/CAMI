---
TASK-ID: TASK-020
NAME: World / Instance
PHASE: Phase 4 · 基础 MMORPG
MODULE: server/gamenode/world
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-012, TASK-018
---

# TASK-020 · World / Instance

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-020` |
| NAME | World / Instance |
| PHASE | Phase 4 · 基础 MMORPG |
| MODULE | `server/gamenode/world` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-012`, `TASK-018` |

---

## 1. Objective

实现 WorldManager / SceneManager 扩展 / InstanceManager，支持 OpenWorld Scene、Dungeon、Arena。实例状态：Pending / Loading / Running / Completed / Destroying。第一版不拆独立服务。

## 2. Dependencies

### 2.1 前置任务

- `TASK-012` · Scene System
- `TASK-018` · NPC / Monster / AI

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 012 018`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/world`

## 4. State Owner（状态归属）

World / Instance 的生命周期与 Scene 归属由 GameNode 内部的 WorldManager / InstanceManager 独占。实例创建、销毁、玩家进出必须走 Command，禁止直接改世界状态。副本内实时状态的 Owner 仍是该副本的 Scene，不归 World 统一持有。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-012 Scene/SceneManager；TASK-018 SpawnDef（副本内怪物生成）

## 6. Output

world 模块（WorldManager / InstanceManager）+ 副本生命周期测试 + 并发实例测试

## 7. Public Interface

```cpp
namespace mmo::game::world {
enum class InstanceState : uint8_t { Pending, Loading, Running, Completed, Destroying };
enum class InstanceType : uint8_t { OpenWorld, Dungeon, Arena, Battleground, TemporaryInstance };
struct InstanceDef { uint32_t def_id; InstanceType type; std::string_view name;
                     uint32_t max_players; uint32_t min_players;
                     DurationMs time_limit{0};       // 0 = 无限
                     std::vector<SpawnDef> spawn_defs;
                     std::string_view scene_asset; };  // 静态配置
struct Instance { InstanceId id; uint32_t def_id; scene::SceneId scene_id;
                  InstanceState state; std::vector<PlayerId> members;
                  core::SteadyTime created_at; core::SteadyTime started_at;
                  DurationMs elapsed; uint32_t version{0}; };
class InstanceManager { public:
  core::Result<InstanceId> Create(uint32_t def_id, std::span<const PlayerId> members, core::TraceID);
  core::Result<void> Start(InstanceId, core::TraceID);
  core::Result<void> Complete(InstanceId, InstanceResult, core::TraceID);
  core::Result<void> Destroy(InstanceId, core::TraceID);
  core::Result<void> AddMember(InstanceId, PlayerId, core::TraceID);
  core::Result<void> RemoveMember(InstanceId, PlayerId, LeaveReason, core::TraceID);
  core::Result<void> Tick(core::SteadyTime now);      // 超时检查、空实例回收
  const Instance* Find(InstanceId) const noexcept;
  size_t CountByState(InstanceState) const noexcept; };  // 指标
class WorldManager { public:
  core::Result<void> Init(const WorldConfig&);
  core::Result<scene::SceneId> GetOrCreateOpenWorld(uint32_t world_def_id);
  core::Result<void> TransferPlayer(PlayerId, scene::SceneId from, scene::SceneId to, core::TraceID);
  core::Result<void> Tick(core::SteadyTime now);
  size_t PlayerCount() const noexcept; size_t SceneCount() const noexcept; };
}
```

## 8. Data Model

**实例状态机**

```
Pending ──> Loading ──> Running ──> Completed ──> Destroying
   │            │           │
   └────────────┴───────────┴──> (超时/全员退出/创建失败) Destroying
```

- **OpenWorld**：常驻，不自动销毁，玩家数可超 max_players（分线）。
- **Dungeon/Arena**：按队伍创建独立实例，全员退出或超时后进入 Destroying（延迟 60 秒兜底，防误杀）。
- 分线策略：OpenWorld 单 Scene 玩家超阈值（默认 300）时自动开分线，分线间互不干扰。

## 9. Thread Model

WorldManager / InstanceManager 由 GameNode 主线程或指定 SimulationThread 驱动。实例的 Scene 由所属 SimulationThread 拥有。跨实例操作（如组队进本）走 Command 队列。

## 10. Hot Path

**NO** （生命周期管理，非每 Tick 热路径；但实例 Tick 会在热路径被遍历）


## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**YES** （实例元数据异步存档）

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/gamenode/world/include/mmo/game/world/
- server/gamenode/world/src/
- server/gamenode/world/tests/
- config/gameplay/world/

## 15. Implementation Steps

1. 定义 instance_def.h：InstanceDef / InstanceState / Instance 结构，全部配置化
2. 实现 instance_manager.h/.cpp：五状态机 + Create/Start/Complete/Destroy/AddMember/RemoveMember
3. 实现实例创建流程：Pending（分配 ID）→ Loading（创建 Scene + 生成怪物/NPC）→ Running（玩家可进入）
4. 实现超时与回收：time_limit 到期转 Completed；全员退出后延迟 60 秒 Destroying；空实例（创建后无人进入 > 5 分钟）自动回收
5. 实现成员管理：上限校验（max_players）、重复加入拒绝、队长离开的处理（第一版：全员退出）
6. 实现 world_manager.h/.cpp：OpenWorld 场景管理、玩家跨场景转移（TransferPlayer 走 Command，禁止直接搬实体）
7. 实现分线：OpenWorld 单 Scene 超阈值自动开分线，分线切换走 TransferPlayer
8. 实现实例指标：各状态实例数、平均实例时长、实例创建/销毁速率
9. 写测试：五状态机全路径与非法转移；创建/加入/退出/完成/销毁；超时回收；空实例回收；人数上限；分线触发
10. 写集成测试：同时开 100 个副本实例（每个 5 人），跑 10 分钟，验证资源回收彻底（Scene 数、实体数归零）

## 16. Unit Test

五状态机全路径；成员增删与上限；超时计算；实例 ID 唯一；配置加载与校验（缺字段报错、引用不存在的 Scene 报错）

## 17. Integration Test

并发开 100 个 5 人副本：全部进入 Running；随机让成员退出，全部成员退出后实例在 60 秒内被回收；跑 10 分钟后 Scene 数与实体数回落到基线（无泄漏）；OpenWorld 300 人触发分线，分线间 AOI 互不可见

## 18. Benchmark

bin/world_bench：`instance_create_us=` / `instance_destroy_us=` / `tick_us_per_100_instances=` / `mem_bytes_per_instance=`

## 19. Failure Test

实例创建时 Scene 创建失败：转 Destroying 并通知成员，不产生悬挂实例；成员在 Loading 阶段退出：实例继续为其他人服务或全员退出后回收（写死策略并测试）；实例超时瞬间玩家正在击杀 BOSS：按配置宽限或强制结算（写死并测试）；100 个实例同时超时：分批回收，不产生 Tick 尖峰（单 Tick 回收上限 10 个）；TransferPlayer 目标 Scene 已满：返回 BUSY，玩家留在原场景

## 20. Acceptance Criteria

1. 五状态（Pending/Loading/Running/Completed/Destroying）全部实现且有单测
2. OpenWorld / Dungeon / Arena 三类场景均可创建运行
3. 100 个并发实例跑 10 分钟后全部回收，Scene 与实体数归零（无泄漏，集成测试断言）
4. 超时、空实例、全员退出三条回收路径均有测试
5. 分线策略生效（300 人触发，分线间不可见）
6. 实例配置全部配置化（代码无硬编码）
7. Debug / Release 双构建通过，ctest -R World 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止把 World/Instance 拆成独立服务（第一版在 GameNode 内）
- 禁止实例无回收（超时、空实例、全员退出必须回收）
- 禁止单 Tick 批量回收导致尖峰（必须分批）
- 禁止 TransferPlayer 直接搬移实体（必须走 Command + Scene Enter/Leave）
- 禁止硬编码副本配置
- 禁止实例创建失败后留下悬挂状态

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

实例创建 < 5ms；销毁 < 3ms；100 个实例 Tick 遍历 < 100us；单实例元数据内存 < 4KB。

## 23. Deliverables

- server/gamenode/world/include/mmo/game/world/instance_manager.h
- server/gamenode/world/include/mmo/game/world/world_manager.h
- server/gamenode/world/src/*.cpp
- server/gamenode/world/tests/*
- config/gameplay/world/*.json
- server/gamenode/world/docs/INTERFACE.md
- server/gamenode/world/docs/README.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-020.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-020.sh
bash scripts/verify/task-020.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 012 018`
2. 交付物存在性检查（4 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R World`
5. Benchmark 执行：`bin/world_bench --instances 100 --duration 600`
6. 性能阈值断言：`bench/world.txt` 中 `mem_bytes_per_instance` ≤ `4096`
7. 性能阈值断言：`bench/world.txt` 中 `tick_us_per_100_instances` ≤ `100`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-020

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): World / Instance

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-020.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-020
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
3. 查依赖：确认 TASK-012, TASK-018 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-020.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-012` · `server/gamenode/scene`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
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
