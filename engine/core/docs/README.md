# engine/core · Core 基础设施（TASK-001 / TASK-002 / TASK-003 / TASK-004）

本模块包含四套相互独立的基础设施：

- **TASK-001 · Core Error / Result** —— `mmo/core/error/*`
- **TASK-002 · Core Logger / Trace** —— `mmo/core/log/*`
- **TASK-003 · Core Time / UUID / Config** —— `mmo/core/{time,uuid,config}/*`

---

# 一、Core Error / Result 系统（TASK-001）

统一的错误处理三件套：`Error`、`Result<T>`、`ErrorCode`。服务端与客户端共用同一套定义，
禁止任何模块自行设计冲突的错误体系。本模块为纯值类型、无运行时状态、无异常、可跨线程自由传递。

## 设计原则

- **禁止异常作为业务错误传播机制**：第三方库异常须在边界转换为 `Error`。
- **失败路径零堆分配**：`Error` 的 `message` 优先存入 32 字节内联缓冲（SSO），短消息下
  构造/复制/传递 `Result` 不触发 `operator new`。
- **`[[nodiscard]]`**：`Result<T>` 与 `Result<void>` 均带 `[[nodiscard]`，丢弃返回值会在
  `-Wall -Wextra` 下产生告警。
- **单一权威错误码表**：9 个标准码由本模块独占维护，其它模块只能引用，禁止自定义第二套
  顶层 `ErrorCode` 枚举。

## 快速开始

```cpp
#include "mmo/core/error/result.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/error.h"

using namespace mmo::core;

// 返回成功
Result<int> load(int id) {
    if (id < 0)
        return Result<int>::Fail(Error(ErrorCode::NOT_FOUND, "bad id", domain::kData));
    return Result<int>::Ok(id);
}

// 链式组合
Result<int> pipeline(int id) {
    auto v = MMO_TRY(load(id));          // 失败提前 return Result<int>::Fail(...)
    return Result<int>::Ok(v * 2);
}

// 断言式使用
void use(int id) {
    auto r = pipeline(id);
    if (r) {
        int v = r.Value();               // 成功取值
    } else {
        Error e = r.Err();               // 失败取错（只读）
        log(e.ToString());               // "data/NOT_FOUND: bad id"
    }
}
```

## 错误码（9 个，值固定、跨进程稳定）

| 值 | 枚举 | 含义 | 可重试 |
|---|---|---|---|
| 0 | `OK` | 成功 | — |
| 1 | `INVALID_ARGUMENT` | 参数非法 | 否 |
| 2 | `NOT_FOUND` | 未找到 | 否 |
| 3 | `TIMEOUT` | 超时 | 是 |
| 4 | `BUSY` | 繁忙 | 是 |
| 5 | `VERSION_CONFLICT` | 版本冲突 | 否 |
| 6 | `UNAUTHORIZED` | 未授权 | 否 |
| 7 | `RATE_LIMITED` | 限流 | 是 |
| 8 | `INTERNAL_ERROR` | 内部错误 | 否 |

`ToString(ErrorCode)` 与 `FromString(std::string_view)` 提供双向映射；`FromString` 对未知名称或
越界数值（如 `"999"`）返回 `std::nullopt`，不崩溃。`IsRetryable(ErrorCode)` 判定可重试。

## 错误域（domain）

`Error.domain` 只允许取受控集合：`core` / `net` / `scene` / `combat` / `data` / `economy` / `lua`
（见 `domain::k*` 常量）。`IsValidDomain(std::string_view)` 用于校验。

## 扩展规则（重要）

**禁止新增顶层 `ErrorCode` 枚举。** 需要扩展错误种类时：

1. 优先复用 9 个标准码（用 `domain` 或 `message` 区分上下文）；
2. 若确需细分错误类型，使用 `Error.domain` 或 `message` 表达，而非新增枚举值；
3. 跨模块扩展点统一走抽象（C++ Interface / Command / Event），新增实现不得修改既有任务文件。

> 模块边界：其它模块只能通过 `engine/core/include/` 下的公开头调用，禁止 `#include` 本模块
> `src/`，禁止访问内部数据。接口在 `STATUS: DONE` 之后变更必须走 `version` + 兼容性评估。

---

# 二、Core Logger / Trace（TASK-002）

统一日志 + 全链路 TraceID。一次请求会在网关、逻辑线程、数据线程之间流转，日志由各自的
`thread_local` 上下文打点，**唯一串联钥匙是 `TraceID`**。

## 设计原则

- **业务线程永不碰磁盘**：只做「格式化 + 无锁入队」，文件 IO 全在后台线程。
- **无锁 MPSC 环形队列**：固定容量，写满即丢弃并计数，**绝不阻塞业务线程**（红线）。
- **关闭日志近乎零成本**：`MMO_LOG` 先判 `ShouldLog` 再格式化，实测 **0.509 ns/次**，
  业务代码可以无条件埋点，不用怕热路径被拖垮。
- **单条日志零堆分配**：格式化走 `per-thread` 栈上缓冲，实测 1000 条日志 0 次 `operator new`。
- **TraceID 可排序、非随机**：`(node_id << 48) | (timestamp_low << 16) | counter`，
  既能按时间排序，又能反查来源节点。

## 快速开始

```cpp
#include "mmo/core/log/logger.h"
#include "mmo/core/log/trace_id.h"

using namespace mmo::core;

// 1) 进程启动时初始化一次
LoggerConfig cfg{};
cfg.service = "gamenode";
cfg.level   = LogLevel::Info;
cfg.console = true;
cfg.file_path = "logs/gamenode.log";
if (!Logger::Init(cfg)) { /* 处理失败 */ }

// 2) 请求入口设置上下文（ScopedLogContext 退出自动恢复）
void OnCastSkill(PlayerID pid, SceneID sid) {
    LogContext ctx{};
    ctx.trace_id  = NewTraceID();
    ctx.request_id = DeriveRequestID(ctx.trace_id);
    ctx.player_id = pid;
    ctx.scene_id  = sid;
    ctx.module    = "skill";            // 必须是静态存储期字面量
    ScopedLogContext scoped(ctx);

    MMO_LOG_INFO("cast skill, id={}", 17);
}

// 3) 跨线程携带：显式拷贝上下文，worker 内 WithContext 套用
void Dispatch(LogContext carried) {
    std::thread([carried] {
        WithContext(carried, [] {
            MMO_LOG_INFO("worker validate target");
        });
    }).detach();
}

// 4) 进程退出前
Logger::Shutdown();
```

## 九项固定字段

| 字段 | 文本模式 | JSON 模式 | 说明 |
|---|---|---|---|
| 时间戳 | 行首 `2026-08-30T00:31:29.333183000Z` | `ts_ns` / `ts` | 纳秒精度，UTC |
| 级别 | `INFO `（5 字符定宽） | `"level":"INFO"` | Trace…Fatal |
| 服务名 | `svc=gamenode` | `"service"` | Gateway / GameNode / … |
| 模块名 | `mod=skill` | `"module"` | 静态字面量，禁止运行时拼接 |
| TraceID | `trace=00010008a39f0000` | `"trace_id"` | **串联一次请求的钥匙** |
| RequestID | `req=00030008a39f0000` | `"request_id"` | 由 TraceID 派生 |
| PlayerID | `player=42` | `"player_id"` | 未设置输出 `-` / `null` |
| SceneID | `scene=7` | `"scene_id"` | 未设置输出 `-` / `null` |
| 消息 | `msg=...` | `"message"` | 最长 `kMaxLogMessage`，超出截断 |
| 线程号 | `tid=3` | `"thread_id"` | 辅助定位跨线程问题 |

> 「九项」是 TASK-002 §20 的表述，实际输出还额外带 `thread_id`，字段只多不少。

## 按 TraceID 串联日志

```bash
# 给定 TraceID，抽出全部相关行并按时间排序（跨线程、跨文件、跨滚动分片）
python tools/logtrace/parse_trace.py 00010008a39f0000 bench/trace_probe.log

# 不传路径时默认扫描 bench/ 与 logs/
python tools/logtrace/parse_trace.py 00010008a39f0000

# 只记得低位片段时用模糊匹配；--json 输出结构化结果供二次处理
python tools/logtrace/parse_trace.py 8a39f0000 --dir logs --fuzzy --json
```

实测输出（`bench/trace_probe.log`，由 `core_log_test` 的 `TestTraceProbe` 生成）：

```
... tid=1 ... msg=probe: main thread begin
... tid=3 ... msg=probe: worker validate
... tid=3 ... msg=probe: worker done, cost_ms=3
... tid=1 ... msg=probe: main thread end
```

退出码：`0` 命中、`1` 未命中（TraceID 写错或日志已滚动删除）、`2` 参数错误。

## 使用规范（踩坑点）

1. **`module` 禁止运行时拼接** —— 拼接必然产生堆分配，直接违反「单条日志零堆分配」。
   用 `static constexpr` 或字符串字面量。
2. **禁止 `std::cout` / `printf` / `std::cerr`** —— 全仓红线，连 `engine/` 下的测试代码
   也必须走 `tests/test_print.h`（内部用 `fwrite`）。
3. **敏感数据禁止入日志** —— 明文口令、令牌、完整身份证 / 银行卡。
4. **`Fatal` 会立即 `Flush()`** —— 用于「进程马上要崩」的场景，不等后台线程。
5. **队列满会丢日志** —— 这是设计取舍，不是 bug。压测期丢包率应 < 0.1%；
   持续丢包说明 sink 是瓶颈，应调大 `queue_capacity` 或降低日志级别。

> 模块边界：下游只能包含 `engine/core/include/mmo/core/log/*`；`src/log/async_ring_buffer.h`
> 与 `src/log/log_formatter.h` 是内部实现，禁止 `#include`。

---

# 三、Core Time / UUID / Config（TASK-003）

三件看起来无关的东西被放在同一个任务里，是因为它们共享同一个约束：
**都在每 Tick / 每次请求的热路径上被调用，且都不允许失败后悄悄降级**。
时钟决定 Tick 的精度，UUID 决定实体身份的唯一性，配置决定运行时可调性。

- **`mmo/core/time/*`** —— 单调时钟（QPC 定点）、墙钟、TickClock（纯整数递推）、`ITimerQueue` 接口
- **`mmo/core/uuid/*`** —— RFC 9562 UUID V4（OS CSPRNG）与 V7（时间前缀，可按时间排序）
- **`mmo/core/config/*`** —— 版本化不可变快照 + 原子整体替换 + 无锁读

## 快速上手

```cpp
#include "mmo/core/time/clock.h"
#include "mmo/core/time/tick_clock.h"
#include "mmo/core/uuid/uuid.h"
#include "mmo/core/config/config_manager.h"

using namespace mmo::core;

// ---- 配置：启动期加载一次 ----
Result<void> loaded = ConfigManager::LoadDir("config");   // app.json / tick.json / network.json
if (!loaded.HasValue()) { /* 启动失败，禁止带病运行 */ }

Result<std::uint32_t> hz = ConfigManager::Get<std::uint32_t>("tick.hz");   // 20
// 缺失 key → NOT_FOUND，消息里带 key 名；类型不匹配 → INVALID_ARGUMENT

// ---- Tick：驱动主循环 ----
TickClock clock(hz.HasValue() ? hz.Value() : TickClock::kDefaultHz);
SteadyTime deadline = MonotonicClock::Point();
while (running) {
    const SteadyTime now = MonotonicClock::Point();
    if (now < deadline) { SleepUntil(deadline); continue; }

    const std::uint32_t steps = clock.CatchUpSteps(now, deadline);  // 最多补 3 个
    for (std::uint32_t i = 0; i < steps; ++i) { world.Tick(); }
    deadline = clock.NextTickDeadline(deadline);   // 纯递推，不读时钟 → 零漂移
}

// ---- UUID：实体身份 ----
Uuid player_id = Uuid::NewV7();       // 可按创建时间排序，适合做数据库主键
Uuid session_id = Uuid::NewV4();      // 纯随机，不泄漏时间信息
std::string text = player_id.ToString();           // "018f...-....-7xxx-....-............"
Result<Uuid> back = Uuid::Parse(text);             // 往返一致
```

## 使用规范（踩坑点）

1. **Tick 一律用 `MonotonicClock` + `TickClock`，永不碰墙钟。**
   `WallClock` 只用于落盘、展示、跨机时间对齐。墙钟会被 NTP 回拨、闰秒、
   虚拟机挂起恢复影响；用它驱动 Tick 会让整服逻辑时间倒流。
2. **`NextTickDeadline` 只在上一帧的 deadline 上加，不要每次读 `MonotonicClock::Now()`。**
   后者会把「本帧超时的那几微秒」累积成漂移；前者跑 10000 Tick 误差实测 0 ns。
3. **`CatchUpSteps` 一定要用，且不要改 `kMaxCatchUpSteps`。**
   帧率跟不上时如果无限补 Tick，会进入「补 Tick → 更慢 → 补更多」的死亡螺旋。
4. **配置热更失败是正常事件，不是崩溃理由。**
   `Reload()` 失败时旧快照原样保留，服务继续跑；调用方应当记 Warn 而不是退出。
5. **重复 key 会直接报 `INVALID_ARGUMENT`。**
   不要写「两个 json 都定义 `tick.hz`，以为后加载的会覆盖」——这是被显式禁止的行为。
6. **`Uuid::NewV4()` / `NewV7()` 失败返回 `Nil()` 而不是抛异常。**
   需要区分「熵源挂了」的场景请用 `TryNewV4()` / `TryNewV7()` 拿 `Result`。
   宁可拿 Nil UUID 让上层显式失败，也不要退化为 `rand()`。
7. **配置读路径虽然无锁，但 `Get<std::string>` 有堆分配**（实测 40.6 ns vs 34.2 ns）。
   每 Tick 都读的字符串配置，应在启动期读一次缓存到局部变量。
8. **`ITimerQueue` 现在只有接口，没有实现。** TASK-004 的 Scheduler 会提供。
   不要自己写一个「差不多」的定时器，等接口实现。

## 目录结构

```
engine/core/
├── include/mmo/core/
│   ├── time/{clock.h, tick_clock.h, timer.h}
│   ├── uuid/uuid.h
│   └── config/config_manager.h
├── src/
│   ├── time/{clock.cpp, wall_clock.cpp, tick_clock.cpp, wall_clock_seam.h}
│   ├── uuid/{uuid.cpp, entropy.h, entropy.cpp}
│   └── config/{json_parser.h, json_parser.cpp, config_snapshot.h, config_manager.cpp}
└── tests/{time_test.cpp, time_bench.cpp}
config/{app.json, tick.json, network.json}
```

> 模块边界：下游只能包含 `engine/core/include/mmo/core/{time,uuid,config}/*`。
> `src/` 下的 4 个内部头（`wall_clock_seam.h`、`entropy.h`、`json_parser.h`、
> `config_snapshot.h`）禁止被下游 `#include`。

---

# 四、Core Memory / Thread / Scheduler（TASK-004）

三件套解决「每 Tick 要往线程池投任务、要定时、要临时内存」的热路径诉求，
且都遵守同一个铁律：**不引入隐藏的线程 / 不引入隐藏的堆分配 / 不因异常拖垮整服**。

- **`mmo/core/thread/*`** —— `TaskFn`（32 字节内联、零堆分配的可调用包装，替代 `std::function`）、
  `MpmcQueue`（Vyukov 风格无锁有界队列，满时返回 false 而非阻塞）、
  `Thread`（四类固定角色 + 有界队列 + 优雅停止）。
- **`mmo/core/sched/scheduler.h`** —— `Scheduler`（最小堆定时器，**被宿主线程驱动**，
  自己不创建任何线程）。
- **`mmo/core/memory/*`** —— `ObjectPool`（分块定长对象池，单线程拥有、无锁）、
  `MemoryPool`（定长块 + 有界跨线程归还）、`Arena`（帧内 bump 分配）。

## 快速上手

```cpp
#include "mmo/core/thread/task.h"
#include "mmo/core/thread/thread.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/memory/object_pool.h"
#include "mmo/core/memory/arena.h"

using namespace mmo::core;

// ---- 任务：提交零堆分配 ----
Thread::Config cfg;
cfg.role = ThreadRole::Worker;
cfg.queue_capacity = 4096;
auto thread = CHECK_OK(Thread::Create(cfg));           // 立即启动
(void)thread->Post(TaskFn([entity_id = 42] { DoWork(entity_id); }));
// 队列满时 Post 返回 ErrorCode::BUSY，不阻塞、不丢任务（调用方自行决定怎么背压）

// ---- 定时器：宿主线程驱动，绝不自带线程 ----
Scheduler sched;
auto sync = CHECK_OK(sched.ScheduleEvery(DurationMs(50), TaskFn([] { TickBuffs(); }))); // 20Hz
// 在 SimulationThread 的主循环里：
const SteadyTime now = MonotonicClock::Point();
const Result<std::size_t> fired = sched.Tick(now);      // 返回本轮触发数
if (!fired.HasValue()) { /* now 倒流（误用墙钟）—— 记告警并退出 */ }

// ---- 内存：Tick 帧内临时对象走 Arena ----
Arena arena(64ULL * 1024 * 1024);                       // 首块 64MB
void* tmp = arena.Push(256, 16);                        // 帧内分配，帧末整块 Reset()
// 实体对象走 ObjectPool（单线程拥有）：
ObjectPool<Player, 4096> players(1024);                 // prewarm 1024 个槽位
Player* p = players.Acquire(player_id);                 // 零堆分配
players.Release(p);                                      // 析构但内存留池
```

## 使用规范（踩坑点）

1. **Scheduler 永远不要自己开线程。** 它只是一个 `Tick(now)` 函数对象，由宿主线程调用。
   验收脚本对 `sched/` 目录做 `std::thread` 字面量扫描（连注释都不行）—— 别在那个目录写线程。
2. **`Tick(now)` 的 `now` 必须来自单调时钟**（`MonotonicClock` / `TickClock`），
   不要传 `WallClock` / 墙钟，否则时间一倒流 `Tick` 直接 `INVALID_ARGUMENT`。
3. **周期定时器 `period <= 0` 会在入口被拦下**（返回 `INVALID_ARGUMENT`），
   不要赌它能在 `Tick` 里被限幅——那会无限触发。
4. **`ObjectPool` / `Arena` 是单线程拥有的，禁止跨线程共享。** 热路径无锁，
   跨线程共用等于静默数据竞争。需要跨线程复用内存用 `MemoryPool`（自带跨线程归还队列）。
5. **`Thread::Post` 队列满返回 `BUSY`，不是丢任务也不是阻塞。** 调用方必须处理背压
   （丢弃 / 合并 / 换线程），不要写 `while (!Post(...).HasValue())` 自旋抢队列。
6. **`PostBlocking` 禁止在 Tick 内调用**（§21 Forbidden）：它会阻塞宿主线程，
   直接拖慢整服 Tick 频率。
7. **`TaskFn` 捕获对象 ≤ 32 字节且构造/移动必须 noexcept。** 超了编译期 `static_assert`
   直接报错；想传大状态就捕获指针或放进对象池。
8. **`MemoryPool::Deallocate` 的野指针 / 双释放只记指标不崩溃**—— 但那说明上层逻辑有 bug，
   别靠它「兜住」错误用法；修复根本原因是正道。

## 目录结构

```
engine/core/
├── include/mmo/core/
│   ├── thread/{task.h, mpmc_queue.h, thread.h}
│   ├── sched/scheduler.h
│   └── memory/{object_pool.h, memory_pool.h, arena.h}
├── src/
│   ├── thread/thread.cpp
│   ├── sched/scheduler.cpp
│   └── memory/{memory_pool.cpp, arena.cpp}
└── tests/thread_test.cpp   （§16 单测 / §17 集成 / §19 Failure）
```

> 模块边界：下游只能包含 `engine/core/include/mmo/core/{thread,sched,memory}/*`。
> `src/thread/thread.cpp`、`src/sched/scheduler.cpp`、`src/memory/*.cpp` 是模块私有实现，
> 禁止被下游 `#include`。
