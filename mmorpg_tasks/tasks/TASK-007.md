---
TASK-ID: TASK-007
NAME: Command / Query / Event Bus
PHASE: Phase 1 · 统一通信
MODULE: engine/core
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-001, TASK-004, TASK-005
---

# TASK-007 · Command / Query / Event Bus

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-007` |
| NAME | Command / Query / Event Bus |
| PHASE | Phase 1 · 统一通信 |
| MODULE | `engine/core` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-001`, `TASK-004`, `TASK-005` |

---

## 1. Objective

实现进程内三条总线：CommandBus（执行操作）、QueryBus（只读查询，无副作用）、EventBus（已发生的事实，异步处理）。提供 Register / Dispatch / Subscribe / Unsubscribe，并跑通 Command→Handler→Event→Subscriber 的 Demo 链路。

## 2. Dependencies

### 2.1 前置任务

- `TASK-001` · Core Error / Result 系统
- `TASK-004` · Core Memory / Thread / Scheduler
- `TASK-005` · Protocol Schema（Protobuf + FlatBuffers）

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 001 004 005`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`engine/core`

## 4. State Owner（状态归属）

CommandBus 独占待处理命令队列，EventBus 独占事件订阅表与派发队列；两者都不拥有业务状态，只负责搬运。命令入队后不可变。事件订阅表只允许在模块注册期修改，运行期只读。任何模块不得自定义第二套消息头。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-001 Result；TASK-004 Scheduler（EventBus 异步派发）；TASK-005 Envelope（命令携带 trace/request id）

## 6. Output

engine/core 的 bus 子模块 + Demo 链路 + 背压与异步派发测试

## 7. Public Interface

```cpp
namespace mmo::core {
// ---- CommandBus：1 command : 1 handler，同步执行，返回结果 ----
template <typename TCommand> class ICommandHandler {
 public: virtual ~ICommandHandler() = default;
  virtual Result<typename TCommand::Result> Handle(const TCommand&, const CommandContext&) = 0; };
class CommandBus {
 public:
  template <typename TCommand, typename H> Result<void> Register(std::shared_ptr<H>);
  template <typename TCommand> Result<typename TCommand::Result> Dispatch(const TCommand&, CommandContext);
  template <typename TCommand, typename Fn> Result<void> RegisterFn(Fn&&);   // 轻量注册
  size_t RegisteredCount() const noexcept; size_t InFlight() const noexcept; };
// ---- QueryBus：只读，编译期+运行期禁止副作用 ----
class QueryBus { public:
  template <typename TQuery, typename Fn> Result<void> RegisterFn(Fn&&);
  template <typename TQuery> Result<typename TQuery::Result> Ask(const TQuery&, QueryContext); };
// ---- EventBus：1 event : N subscriber，异步 ----
class EventBus { public:
  using SubId = uint64_t;
  template <typename TEvent> Result<SubId> Subscribe(std::function<void(const TEvent&)>);
  Result<void> Unsubscribe(SubId);
  template <typename TEvent> Result<void> Publish(const TEvent&);           // 入队，不立即执行
  template <typename TEvent> Result<void> PublishImmediate(const TEvent&);  // 同线程立即派发
  Result<size_t> Drain(size_t max_events, DurationMs budget);               // 由宿主线程驱动
  size_t QueueDepth() const noexcept; size_t DroppedCount() const noexcept; };
struct CommandContext { TraceID trace_id; RequestID request_id; PlayerID player_id; SceneID scene_id; };
}
```

## 8. Data Model

| 概念 | 语义 | 返回 | 副作用 |
|---|---|---|---|
| Command | 「请执行一个操作」 | Result&lt;T&gt; | 允许，且必须可审计 |
| Query | 「读取数据」 | Result&lt;T&gt; | **禁止** |
| Event | 「已经发生的事实」 | void | 由订阅者决定，发布者不关心 |

**Command 必带字段**：RequestID / PlayerID / Source / Timestamp / Version；经济类额外带 TransactionID / IdempotencyKey。
**Event 默认异步**，由宿主线程 Drain 派发，禁止在 Tick 中间无限派发。

## 9. Thread Model

CommandBus / QueryBus 在**调用者线程**同步执行（通常是 SimulationThread）。EventBus 入队无锁（MPMC，来自 TASK-004），Drain 由 SimulationThread 在 Tick 的 Event 阶段调用并**带时间预算**，超时留到下帧。禁止 EventBus 自带线程。

## 10. Hot Path

**YES** （Command/Event 位于 Tick 热路径）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- engine/core/include/mmo/core/bus/
- engine/core/src/bus/
- engine/core/tests/
- engine/core/benchmark/
- engine/core/docs/

## 15. Implementation Steps

1. 实现 bus/command.h：Command 概念约束（必须含 RequestID 等字段，用 concept 静态断言）
2. 实现 bus/command_bus.h/.cpp：类型安全的 Handler 注册（std::type_index → 类型擦除的 handler 槽），Dispatch 返回结果，未注册返回 NOT_FOUND
3. 实现 bus/query_bus.h/.cpp：同构实现，Ask 只读；加编译期标注与运行期文档约束「禁止副作用」
4. 实现 bus/event_bus.h/.cpp：MPMC 队列 + 类型擦除订阅表；Publish 入队；Drain(now, max_events, budget) 批量派发
5. 实现背压策略：队列满时按配置丢弃**非关键**事件并计数（DroppedCount 指标），关键事件（经济类）禁止丢弃，改为返回 BUSY 让调用方处理
6. 实现 Unsubscribe 幂等；订阅者抛错时捕获记录并继续派发给其他订阅者（禁止一个坏订阅者拖垮总线）
7. 实现 Drain 时间预算：单个 Tick 中 Event 阶段默认 2ms 预算，超时立即返回剩余数量
8. 写 Demo（tests/demo_pipeline.cpp）：Dispatch(MovePlayerCommand) → Handler 处理 → Publish(PlayerMovedEvent) → 两个 Subscriber 收到 → 断言顺序与内容
9. 写测试：未注册 Command 返回 NOT_FOUND；Query 无副作用（在 handler 中试图修改状态 → 通过测试替身检测到并失败）；Event 多订阅者按注册顺序派发；Unsubscribe 后不再收到；Drain 预算生效
10. 写 benchmark：1e6 次 Command Dispatch、1e6 次 Event Publish+Drain
11. 写 docs/README.md（Command/Query/Event 选型决策树）与 docs/INTERFACE.md

## 16. Unit Test

Command 注册/重复注册（返回错误）/未注册 Dispatch；Query Ask 与错误传播；Event 订阅/退订/多播；Unsubscribe 幂等；Drain 预算与剩余计数；订阅者异常隔离；背压丢弃计数

## 17. Integration Test

Demo 全链路：Command → Handler → Event → 2 个 Subscriber，断言执行顺序与 payload 一致；跨线程（Worker 投递 Command 到 Simulation）验证上下文（trace_id）不丢失；1e5 次混合负载下无死锁、无丢事件（关键事件零丢弃）

## 18. Benchmark

bin/bus_bench：`cmd_dispatch_ns=` / `event_publish_ns=` / `event_drain_ns_per_event=` / `alloc_per_cmd=`

## 19. Failure Test

Handler 抛异常：捕获转成 INTERNAL_ERROR，总线状态不变可继续服务；Subscriber 抛异常：记录日志 + 指标，其余订阅者照常收到；队列满：非关键事件丢弃并计数，关键事件返回 BUSY；Drain 预算耗尽：返回剩余数量且不阻塞；重复注册同一 Command：返回错误而非覆盖（防静默覆盖——这是 CAMI 踩过的坑）

## 20. Acceptance Criteria

1. Demo 链路跑通：Dispatch(Command) → Handler → Publish(Event) → Subscriber 收到（集成测试断言）
2. Query 无副作用：测试替身可检测到任何写操作并判定失败
3. EventBus **不创建线程**（grep 验证），Drain 由宿主驱动且带时间预算
4. 队列满时：非关键事件丢弃并计数，关键事件返回 BUSY，行为可被单测断言
5. 一个订阅者抛异常不影响其他订阅者（单测覆盖）
6. 重复注册同一 Command 类型返回错误，不静默覆盖
7. benchmark 指标达标并写入 docs/PERFORMANCE.md
8. Debug / Release 双构建通过，ctest -R Core_Bus 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止 EventBus 自带线程（必须宿主 Drain）
- 禁止在 Query handler 中修改任何状态
- 禁止在 Tick 中间无限派发事件（必须带预算）
- 禁止让一个异常的订阅者中断整个事件派发
- 禁止静默覆盖已注册的 Handler（必须返回错误）
- 禁止把进程内模块调用改造成网络 RPC
- 禁止丢关键（经济类）事件

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Command Dispatch < 150ns；Event Publish < 100ns；Event Drain < 80ns/event；单次 Command 处理堆分配 = 0（走 Arena/Pool）；1e6 事件队列内存占用 < 64MB。

## 23. Deliverables

- engine/core/include/mmo/core/bus/command_bus.h
- engine/core/include/mmo/core/bus/query_bus.h
- engine/core/include/mmo/core/bus/event_bus.h
- engine/core/src/bus/*.cpp
- engine/core/tests/demo_pipeline.cpp
- engine/core/tests/*
- engine/core/benchmark/*
- engine/core/docs/README.md
- engine/core/docs/INTERFACE.md
- engine/core/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-007.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-007.sh
bash scripts/verify/task-007.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 001 004 005`
2. 交付物存在性检查（7 项）
3. 静态红线扫描：`engine/core/src/bus` 内禁止出现 /std::thread/
4. CMake configure + 编译（Debug + Release 双构建）
5. ctest 过滤执行：`-R Core_Bus`
6. Benchmark 执行：`bin/bus_bench --iterations 1000000`
7. 性能阈值断言：`bench/core_bus.txt` 中 `cmd_dispatch_ns` ≤ `150`
8. 性能阈值断言：`bench/core_bus.txt` 中 `event_drain_ns_per_event` ≤ `80`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-007

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(core): Command / Query / Event Bus

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-007.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-007
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
3. 查依赖：确认 TASK-001, TASK-004, TASK-005 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-007.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-001` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-004` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-005` · `protocol`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
