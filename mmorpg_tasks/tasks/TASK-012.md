---
TASK-ID: TASK-012
NAME: Scene System
PHASE: Phase 3 · GameNode Core
MODULE: server/gamenode/scene
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-011
---

# TASK-012 · Scene System

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-012` |
| NAME | Scene System |
| PHASE | Phase 3 · GameNode Core |
| MODULE | `server/gamenode/scene` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-011` |

---

## 1. Objective

实现 Scene 与 SceneManager：场景生命周期（Creating/Loading/Running/Draining/Destroying）与场景标识（SceneID / SceneVersion / TickNumber / StateHash / OwnerGameNode）。Scene 是实时仿真的基本单位。

## 2. Dependencies

### 2.1 前置任务

- `TASK-011` · Entity System

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 011`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/scene`

## 4. State Owner（状态归属）

Scene 是实时状态的权威 Owner。SceneID / SceneType / TickNumber / SceneVersion / StateHash 以及 Scene 内所有 Entity 的位置、HP、Buff，只有持有该 Scene 的 GameNode Simulation 线程可写。Redis 与 MySQL 只持有非权威副本。Scene 迁移时 Owner 单点移交，任何时刻禁止双写。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-011 EntityManager；PROJECT_REQUIREMENTS.md 第 11/12 节 Scene 与 Ownership 模型

## 6. Output

scene 模块 + 生命周期测试 + 场景上下文测试

## 7. Public Interface

```cpp
namespace mmo::game {
enum class SceneState : uint8_t { Creating, Loading, Running, Draining, Destroying };
enum class SceneType : uint8_t { World, Dungeon, Arena, Battleground, TemporaryInstance };
struct SceneContext {                    // 传给各系统的运行期上下文，禁止反向依赖 Scene 私有成员
  SceneId id; SceneType type; NodeId owner_node;
  core::SteadyTime now; uint64_t tick_number;
  entity::EntityManager& entities; core::EventBus& events; core::Scheduler& scheduler;
  core::Arena& frame_arena;              // 帧内分配，Tick 结束自动 Reset
};
class Scene { public:
  SceneId Id() const noexcept; SceneType Type() const noexcept; SceneState State() const noexcept;
  uint32_t Version() const noexcept; uint64_t TickNumber() const noexcept; uint64_t StateHash() const noexcept;
  NodeId OwnerNode() const noexcept;
  core::Result<void> Enter(PlayerId, entity::EntityId avatar);
  core::Result<void> Leave(PlayerId, LeaveReason);
  core::Result<void> Tick(const SceneContext&);    // 由 TASK-013 调度器驱动，本任务只留接口
  size_t PlayerCount() const noexcept; size_t EntityCount() const noexcept;
  core::Result<void> TransitionTo(SceneState);     // 非法转移返回错误
  core::Result<void> ComputeStateHash();           // 为 TASK-037 恢复与回放校验预留
};
class SceneManager { public:
  core::Result<Scene*> Create(SceneId, SceneType, NodeId owner);
  core::Result<Scene*> Find(SceneId) noexcept;
  core::Result<void> Destroy(SceneId);
  core::Result<void> TickAll(core::SteadyTime now);   // 顺序遍历，禁止并发 Tick 同一 Scene
  std::vector<Scene*> All() const; size_t Count() const noexcept; };
}
```

## 8. Data Model

**Scene 状态机**

```
Creating ──> Loading ──> Running ──> Draining ──> Destroying
    │           │           │            │
    └───────────┴───────────┴────────────┴──> (失败路径) Destroying
```

- `TickNumber` 单调递增，跨 Scene 独立。
- `StateHash` 每 N Tick（默认 60，可配）计算一次，用于确定性与恢复校验；计算走增量哈希，禁止全量序列化。
- `OwnerGameNode` 在创建时确定，第一版**不迁移**。

## 9. Thread Model

每个 Scene 由**唯一** Simulation 线程顺序 Tick（单 Owner），不同 Scene 可并行于不同线程。禁止两个线程同时 Tick 同一 Scene。跨 Scene 交互只通过 Command/Event 队列。

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

- server/gamenode/scene/include/mmo/game/scene/
- server/gamenode/scene/src/
- server/gamenode/scene/tests/
- server/gamenode/scene/docs/

## 15. Implementation Steps

1. 定义 scene_id.h：SceneId 编码（type + index），保证全局唯一
2. 实现 scene.h/.cpp：Scene 结构、五状态机、Enter/Leave、生命周期事件（SceneCreated/SceneLoaded/SceneRunning/SceneDraining/SceneDestroyed）
3. 实现 scene_context.h：SceneContext 聚合各系统引用，明确「系统只通过 Context 访问，禁止反向持有 Scene 指针」
4. 实现 scene_manager.h/.cpp：创建/查找/销毁/TickAll，Scene 表用读写锁保护（只在创建销毁时写，Tick 遍历时读快照）
5. 实现 Enter/Leave：进入时创建 Avatar 实体、绑定 PlayerID→EntityID 映射；离开时延迟销毁实体并发布事件
6. 实现 StateHash 增量计算：对关键状态（实体位置、HP、Buff 数量）做滚动哈希，每 N Tick 一次
7. 实现场景容量上限：单 Scene 最大实体数与玩家数，超限拒绝 Enter 并返回 BUSY
8. 实现 Scene 内实体索引：PlayerID→EntityID 的 O(1) 映射表（有界）
9. 写测试：状态机全路径与非法转移；Enter/Leave 与事件；StateHash 稳定性（相同操作序列产生相同 hash）；容量上限；并发 TickAll 不重入同一 Scene
10. 写集成测试：创建 3 个 Scene（World/Dungeon/Arena），各进出 100 名玩家，跑 1000 Tick 无异常

## 16. Unit Test

五状态机合法/非法转移；SceneId 唯一；Enter/Leave 与实体绑定；StateHash 确定性与增量更新；容量上限；SceneContext 只读语义

## 17. Integration Test

3 类型 Scene 各 100 玩家进出 + 1000 Tick 长稳：无泄漏、状态正确、事件齐全；TickAll 遍历 100 个 Scene 耗时可测；Draining 期间禁止新玩家进入（断言拒绝）

## 18. Benchmark

bin/scene_bench：`scene_tick_overhead_ns=` / `enter_ns=` / `leave_ns=` / `state_hash_us_per_1k_entities=` / `mem_bytes_per_scene=`

## 19. Failure Test

Tick 中途 Scene 被销毁：延迟到 Tick 结束，不崩溃；Enter 超容量：返回 BUSY 并有指标；StateHash 计算超时：跳过本轮并记录（禁止拖慢 Tick）；玩家重复 Enter：幂等或返回错误（写死一种并测试）；SceneManager 并发创建同 Id：第二个返回 VERSION_CONFLICT

## 20. Acceptance Criteria

1. Scene 五状态机全部路径有单测，非法转移返回错误
2. Scene 必含 SceneID / SceneVersion / TickNumber / StateHash / OwnerGameNode 五项（grep 结构体验证）
3. Enter/Leave 正确创建/销毁 Avatar 实体并发布事件
4. StateHash 在相同操作序列下可复现（确定性，单测断言两次结果一致）
5. 单 Scene 只能被一个线程 Tick（代码评审 + 并发测试）
6. 容量上限生效，超限返回 BUSY
7. Debug / Release 双构建通过，ctest -R Scene 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止两个线程并发 Tick 同一 Scene
- 禁止系统反向持有 Scene 私有成员（只走 SceneContext）
- 禁止无界的 Scene 玩家/实体数量
- 禁止在 Tick 内同步销毁 Scene
- 禁止把 Scene 状态写进 Redis 作为权威数据源（Redis 不是实时 Owner）
- 禁止 StateHash 使用全量序列化导致 Tick 抖动

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Scene Tick 框架开销（空 Scene）< 1us；单 Scene 基础内存 < 64KB；Enter < 5us；Leave < 5us；StateHash（1000 实体）< 200us 且默认每 60 Tick 一次。

## 23. Deliverables

- server/gamenode/scene/include/mmo/game/scene/scene.h
- server/gamenode/scene/include/mmo/game/scene/scene_context.h
- server/gamenode/scene/include/mmo/game/scene/scene_manager.h
- server/gamenode/scene/src/*.cpp
- server/gamenode/scene/tests/*
- server/gamenode/scene/docs/INTERFACE.md
- server/gamenode/scene/docs/README.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-012.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-012.sh
bash scripts/verify/task-012.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 011`
2. 交付物存在性检查（1 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Scene`
5. Benchmark 执行：`bin/scene_bench --scenes 100 --ticks 1000`
6. 性能阈值断言：`bench/scene.txt` 中 `scene_tick_overhead_ns` ≤ `1000`
7. 性能阈值断言：`bench/scene.txt` 中 `mem_bytes_per_scene` ≤ `65536`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-012

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Scene System

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-012.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-012
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
3. 查依赖：确认 TASK-011 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-012.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-011` · `server/gamenode/entity`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
