---
TASK-ID: TASK-004
NAME: Core Memory / Thread / Scheduler
PHASE: Phase 0 · 工程基础
MODULE: engine/core
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-001, TASK-003
---

# TASK-004 · Core Memory / Thread / Scheduler

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-004` |
| NAME | Core Memory / Thread / Scheduler |
| PHASE | Phase 0 · 工程基础 |
| MODULE | `engine/core` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-001`, `TASK-003` |

---

## 1. Objective

建立线程模型与执行骨架：Thread 抽象、Task 提交、Scheduler 定时任务、ObjectPool / MemoryPool / Arena 内存设施，并固定四类线程（Network / Simulation / Worker / Persistence）。

## 2. Dependencies

### 2.1 前置任务

- `TASK-001` · Core Error / Result 系统
- `TASK-003` · Core Time / UUID / Config

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 001 003`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`engine/core`

## 4. State Owner（状态归属）

Scheduler 独占 Timer 表与任务队列的写入权，只允许在 Tick Safe Point 或指定 Worker 线程上调度。ObjectPool / Arena / MemoryPool 的 Owner 是使用方线程（每 Worker 独占一份，禁止跨线程共享同一个池）。禁止本层依赖任何 Gameplay 模块。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-001 Error/Result；TASK-003 MonotonicClock（定时器基准）

## 6. Output

thread / scheduler / memory 三个子模块 + 并发压测 + ObjectPool & Scheduler benchmark

## 7. Public Interface

```cpp
namespace mmo::core {
enum class ThreadRole : uint8_t { Network, Simulation, Worker, Persistence };
class Thread { public:
  struct Config { ThreadRole role; std::string name; size_t queue_capacity{4096}; };
  static Result<std::unique_ptr<Thread>> Create(Config, std::function<void()> on_start = {});
  Result<void> Post(TaskFn);                 // 非阻塞入队，队列满返回 BUSY
  Result<void> PostBlocking(TaskFn);
  void RequestStop() noexcept; void Join();
  ThreadRole Role() const noexcept; size_t Pending() const noexcept;   // 队列深度指标
};
class Scheduler { public:                    // 基于 TASK-003 TickClock 的单调定时器
  using TimerId = uint64_t;
  Result<TimerId> ScheduleAfter(DurationMs, TaskFn);
  Result<TimerId> ScheduleAt(SteadyTime, TaskFn);
  Result<TimerId> ScheduleEvery(DurationMs, TaskFn);   // 周期任务，用于 Buff Tick
  Result<void>    Cancel(TimerId);
  size_t ReadyCount() const noexcept;        // 到期待执行数量（指标）
  Result<size_t>  Tick(SteadyTime now);      // 由宿主线程驱动，**不自带线程**
};
template <typename T, size_t Chunk = 4096> class ObjectPool {
 public:
  explicit ObjectPool(size_t prewarm = 0);
  T* Acquire(); void Release(T*) noexcept;
  size_t Capacity() const noexcept; size_t InUse() const noexcept;
};
class MemoryPool { public:                   // 定长块分配，无全局锁
  void* Allocate(size_t bytes); void Deallocate(void*, size_t) noexcept;
  size_t UsedBytes() const noexcept; size_t ChunkSize() const noexcept; };
class Arena { public:                        // 帧内分配，整块重置
  explicit Arena(size_t bytes); void* Push(size_t bytes, size_t align = 8);
  void Reset() noexcept; size_t UsedBytes() const noexcept; };
}
```

## 8. Data Model

| 类型 | 说明 |
|---|---|
| ThreadRole | Network / Simulation / Worker / Persistence，四类角色固定，禁止新增第五类不打招呼 |
| TaskFn | `std::function<void()>` 的替代品：小对象优化的 `fu2::function` 或自定义 32 字节内联缓冲，**避免每次提交都堆分配** |
| TimerId | 单调递增 uint64，Cancel 幂等 |
| ObjectPool&lt;T&gt; | 自由链表 + 分块扩容，Release 后对象析构但内存不归还 OS |

## 9. Thread Model

Scheduler **不自带线程**，必须由宿主线程调用 `Tick(now)`（SimulationThread 驱动游戏定时器，WorkerThread 驱动后台定时器），从根本上避免「每个 Buff 一个 Timer 线程」。ObjectPool/MemoryPool 用 thread_local 缓存 + 全局后备，热路径无锁。

## 10. Hot Path

**YES** （ObjectPool 与 Scheduler 位于 Tick 热路径）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- engine/core/include/mmo/core/thread/
- engine/core/include/mmo/core/sched/
- engine/core/include/mmo/core/memory/
- engine/core/src/{thread,sched,memory}/
- engine/core/tests/
- engine/core/benchmark/

## 15. Implementation Steps

1. 实现 thread/task.h：TaskFn 类型（小对象优化，32 字节内联，避免堆分配）
2. 实现 thread/thread.h/.cpp：Thread 封装（角色命名、队列容量、优雅停止、Pending 指标）
3. 实现 thread/mpmc_queue.h：无锁 MPMC 队列（Vyukov 风格），单测验证多生产者多消费者正确性
4. 实现 sched/scheduler.h/.cpp：四级时间轮或最小堆（选最小堆，实现简单且 Cancel 高效）+ Tick(now) 驱动 + 到期任务批量出队
5. 实现 Cancel 幂等与已取消任务的资源回收（禁止内存泄漏）
6. 实现 memory/object_pool.h：分块自由链表，支持 prewarm，提供 InUse/Capacity 指标
7. 实现 memory/memory_pool.h：定长块池，thread_local free list + 全局后备，跨线程归还有界
8. 实现 memory/arena.h：bump allocator，Reset 整块回收，用于 Tick 帧内临时对象
9. 写并发测试：4 生产者 4 消费者各 10 万任务，验证无丢失、无重复、无死锁；Scheduler 1000 个周期定时器精度测试
10. 写 benchmark：ObjectPool acquire/release、MemoryPool alloc/free、Scheduler 1 万定时器 Tick 耗时
11. 写 docs/DEPENDENCY.md 与 docs/PERFORMANCE.md，明确 Scheduler 的线程归属红线

## 16. Unit Test

MPMC 队列单/多生产者消费者正确性与顺序；Thread 启停与优雅 Join；Scheduler 一次性/周期/取消/幂等 Cancel；ObjectPool Acquire/Release 不泄漏（ASan 下跑）；MemoryPool 大小对齐；Arena Reset 后可复用

## 17. Integration Test

四类线程各起一个实例，Network 投递任务到 Worker，Worker 回投到 Simulation，跑 10 秒无死锁无丢任务；Scheduler 挂在 SimulationThread 上驱动 500 个 20Hz 周期任务，实测每秒触发 10000 ± 50 次

## 18. Benchmark

bin/sched_bench：`sched_tick_us_10k_timers=`；bin/mem_bench：`pool_acquire_release_ns=` / `arena_push_ns=` / `mempool_alloc_ns=`

## 19. Failure Test

任务队列满：Post 返回 BUSY 而非阻塞或丢任务；Scheduler 到期任务抛错：捕获记录并继续，不影响后续定时器；ObjectPool 耗尽：按配置扩容或返回 nullptr（禁止崩溃）；线程 Join 超时：记录告警并上报指标；跨线程归还 MemoryPool 块：不崩溃、不双释放（ASan 验证）

## 20. Acceptance Criteria

1. Scheduler **不创建任何线程**（grep 验证：`std::thread` 不出现在 sched/ 目录）
2. 4×4 并发 10 万任务无丢失、无重复、无死锁
3. Scheduler 挂 500 个 20Hz 周期任务，10 秒内触发次数误差 < 1%
4. ObjectPool / MemoryPool / Arena 在 ASan 下无泄漏、无越界
5. benchmark 四项指标达标并写入 docs/PERFORMANCE.md
6. 每类线程暴露 Pending/ReadyCount 指标（供 TASK-039 指标采集）
7. Debug / Release 双构建通过，ctest -R Core_Thread 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止 Scheduler 自带线程（必须宿主驱动）
- 禁止为每个 Buff / 每个定时任务创建一个线程或一个 OS timer
- 禁止在热路径使用 std::function 造成堆分配（用 TaskFn）
- 禁止在 Tick 内做阻塞式 PostBlocking
- 禁止使用全局锁保护 ObjectPool 的热路径分配
- 禁止在 Tick 内 new/delete 大对象（应走 Pool / Arena）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

ObjectPool acquire+release < 20ns；MemoryPool alloc < 15ns；Arena push < 3ns；Scheduler Tick（1 万定时器）< 200us；任务投递 < 100ns。

## 23. Deliverables

- engine/core/include/mmo/core/thread/thread.h
- engine/core/include/mmo/core/thread/mpmc_queue.h
- engine/core/include/mmo/core/sched/scheduler.h
- engine/core/include/mmo/core/memory/object_pool.h
- engine/core/include/mmo/core/memory/memory_pool.h
- engine/core/include/mmo/core/memory/arena.h
- engine/core/src/{thread,sched,memory}/*.cpp
- engine/core/tests/*
- engine/core/benchmark/*
- engine/core/docs/DEPENDENCY.md
- engine/core/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-004.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-004.sh
bash scripts/verify/task-004.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 001 003`
2. 交付物存在性检查（2 项）
3. 静态红线扫描：`engine/core/src/sched` 内禁止出现 /std::thread/
4. 静态红线扫描：`engine/core/include/mmo/core/sched` 内禁止出现 /std::thread/
5. CMake configure + 编译（Debug + Release 双构建）
6. ctest 过滤执行：`-R Core_Thread`
7. Benchmark 执行：`bin/sched_bench --timers 10000 --ticks 1000`
8. Benchmark 执行：`bin/mem_bench --ops 10000000`
9. 性能阈值断言：`bench/core_sched.txt` 中 `sched_tick_us_10k_timers` ≤ `200`
10. 性能阈值断言：`bench/core_mem.txt` 中 `pool_acquire_release_ns` ≤ `20`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-004

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(core): Core Memory / Thread / Scheduler

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-004.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-004
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
3. 查依赖：确认 TASK-001, TASK-003 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-004.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-001` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-003` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
