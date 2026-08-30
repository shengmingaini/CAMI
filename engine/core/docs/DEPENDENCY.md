# engine/core · 依赖说明（TASK-001 / TASK-002 / TASK-003 / TASK-004 / TASK-007）

> 五文档契约之一。本文件回答三个问题：**本模块依赖谁**、**谁依赖本模块**、
> **哪些依赖是被禁止的**。

## 一、模块定位

```
Game → Gameplay → Core
                ↑
            engine/core（本模块）
```

`engine/core` 是**依赖图的叶子**：它不依赖 Game / Gameplay，也不依赖任何业务模块。
所有上层模块都可以依赖 core，core 永远不能反向依赖上层。

## 二、外部依赖（第三方 / 系统）

| 依赖 | 用途 | 引入任务 | 离线可用性 | 备注 |
|---|---|---|---|---|
| C++20 标准库 | 全部 | TASK-001 | ✅ | `<format>` / `<string_view>` / `<atomic>` / `<shared_mutex>` |
| Windows `bcrypt` (`BCryptGenRandom`) | UUID 熵源 | TASK-003 | ✅ | 仅 Windows 分支链接；POSIX 走 `getrandom` / `/dev/urandom` |
| Windows `QueryPerformanceCounter` | 单调时钟 | TASK-003 | ✅ | 系统 API，无需链接额外库 |
| Windows `GetSystemTimePreciseAsFileTime` | 墙钟 | TASK-003 | ✅ | POSIX 走 `clock_gettime(CLOCK_REALTIME)` |
| pthread | 测试线程 | TASK-002 | ✅ | MinGW-w64 自带 |

**vcpkg 依赖：无。** `vcpkg.json` 的 `dependencies` 目前仍为空数组，
baseline `aae277ac`。TASK-003 刻意没有引入 nlohmann/json 等第三方 JSON 库 ——
配置模块只需要「读扁平 key-value」，自研 300 行递归下降解析器即可，
且vcpkg 在 GFW 下离线拉不到包。后续若需要 JSON **序列化**（写配置），
再走「按需声明、离线可绿」流程引入。

## 三、内部依赖（模块内）

```
mmo_core_error            (TASK-001)
    ↑
    ├── mmo_core_log      (TASK-002)   → error
    ├── mmo_core_time     (TASK-003)   → error
    ├── mmo_core_uuid     (TASK-003)   → error, time
    ├── mmo_core_config   (TASK-003)   → error
    ├── mmo_core_thread   (TASK-004)   → error, time
    ├── mmo_core_sched    (TASK-004)   → error, time
    ├── mmo_core_memory   (TASK-004)   → error
    └── mmo_core_bus      (TASK-007)   → error, log, thread, time
```

| 目标 | 依赖 | CMake 链接 | 说明 |
|---|---|---|---|
| `mmo_core_error` | — | — | 依赖图的根，无任何依赖 |
| `mmo_core_log` | `mmo::core_error` | PUBLIC | 日志的 Error 传播 |
| `mmo_core_time` | `mmo::core_error` | PUBLIC | 时钟本身几乎不会失败，仅 `TimerSpec` 校验用 Error |
| `mmo_core_uuid` | `mmo::core_error`, `mmo::core_time` | PUBLIC | V7 需要毫秒时间戳 |
| `mmo_core_config` | `mmo::core_error` | PUBLIC | 解析/加载失败返回 Error |
| `mmo_core_thread` | `mmo::core_error`, `mmo::core_time` | PUBLIC | `Thread` 用 `MpmcQueue` + `TaskFn`；`Post` 用到 `ErrorCode::BUSY` |
| `mmo_core_sched` | `mmo::core_error`, `mmo::core_time` | PUBLIC | `Scheduler::Tick` 用 `MonotonicClock` 判时间倒流 |
| `mmo_core_memory` | `mmo::core_error` | PUBLIC | `MemoryPool` 跨线程归还需 `MpmcQueue` |
| `mmo_core_bus` | `mmo::core_error`, `mmo::core_log`, `mmo::core_thread`, `mmo::core_time` | PUBLIC | Command/Query 失败返回 `Error`；Context 复用 `log` 的 ID 类型；Event 队列用 `thread` 的 `MpmcQueue`；Drain 用 `time` 的单调时钟做时间预算 |
| `core_time_test` | time, uuid, config | PRIVATE | 测试可执行件，不被下游链接 |
| `core_thread_test` | memory, thread, sched, time, error | PRIVATE | TASK-004 测试可执行件 |
| `core_bus_test` | bus, error, log, thread, time | PRIVATE | TASK-007 单测/集成/Failure 可执行件 |
| `core_bus_demo` | bus, error, log, thread, time | PRIVATE | TASK-007 演示链路可执行件（Command → Event → 2 订阅者） |
| `time_bench` | time, uuid, config | PRIVATE | benchmark 可执行件 |
| `sched_bench` | sched, time | PRIVATE | TASK-004 scheduler benchmark |
| `mem_bench` | memory, time | PRIVATE | TASK-004 memory benchmark |
| `bus_bench` | bus, error, log, thread, time | PRIVATE | TASK-007 benchmark（1e6 次，产出 bench/core_bus.txt） |

**注意**：`mmo_core_config` **不依赖** `mmo_core_time`。
配置快照的版本号是 `std::atomic<std::uint64_t>` 自增，不需要读时钟，
避免把时钟拖进配置的热路径。

## 三之二、TASK-004 子模块依赖与线程归属（关键红线）

```
mmo_core_thread  ──┐  MpmcQueue + TaskFn（无锁任务队列与轻量可调用）
mmo_core_memory   ─┤  ObjectPool / MemoryPool / Arena（单线程拥有的内存设施）
mmo_core_sched    ─┘  Scheduler（最小堆定时器，宿主线程驱动）
```

- **`mmo_core_sched` 不依赖 `mmo_core_thread`**：Scheduler 自己**不创建、不拥有任何执行线程**。
  它只是一个被宿主线程调用的 `Tick(now)` 函数对象。验收脚本对 `engine/core/src/sched`
  与 `engine/core/include/mmo/core/sched` 做 `std::thread` 字面量静态扫描，
  **连注释里都不能出现那个类型名**。定时器由 SimulationThread（驱动游戏逻辑定时器）或
  WorkerThread（驱动后台定时器）在自己的循环里调用 `Tick` 驱动。
- **`mmo_core_thread` 的 `Thread` 是执行体**：它创建线程、跑 `RunLoop` 消费 `MpmcQueue`。
  四类角色（Network / Simulation / Worker / Persistence）固定，禁止私自新增第五类。
- **内存三件套都是单线程拥有的**：`ObjectPool` / `Arena` 全程无锁无原子；
  `MemoryPool` 拥有者线程走无锁空闲链表，其他线程归还走有界 `MpmcQueue`（冷路径）。
  禁止把 `ObjectPool` 跨线程共享（会数据竞争）。
- 三者之间无循环依赖：`thread` 不依赖 `sched`/`memory`，`sched`/`memory` 不依赖 `thread`。

## 三之三、TASK-007 总线依赖与线程归属（关键红线）

```
mmo_core_bus  ──┬→ mmo_core_error   Command / Query / Event 的失败语义（Result / ErrorCode）
                ├→ mmo_core_log     复用 TraceID / RequestID / PlayerID / SceneID（不新建 ID 体系）
                ├→ mmo_core_thread  EventBus 队列复用 MpmcQueue（Vyukov 无锁有界队列）
                └→ mmo_core_time    Drain 的时间预算用 MonotonicClock
```

- **`mmo_core_bus` 不依赖 `protocol` / RPC 模块**：Context 直接复用 TASK-002 的
  `TraceID` / `RequestID`（与 TASK-005 Envelope 同源），总线因此保持「叶子依赖」，
  Core 永不反向依赖上层。
- **EventBus 不创建、不拥有任何执行线程**：`Publish` 只入队，`Drain` 由宿主线程
  显式调用。验收脚本对 `engine/core/src/bus` 做 `std::thread` 字面量静态扫描。
- **CommandBus / QueryBus 假设单线程（SimulationThread）使用**：注册期可写、
  运行期只读，读路径无锁；跨线程调用需自行串行化。EventBus 的 `Publish` 是
  线程安全的（MPMC 队列 + 订阅表 shared_mutex）。
- **`event_slot.h` 是总线私有类型擦除实现**：`EventTypeInfo` 是进程级静态描述符，
  槽位下标在 `EventBus` 实例内 —— 二者不得混放（已实测跨实例 SIGSEGV）。

## 四、被依赖方（谁会用到本模块）

按 TASK-004 起的规划，预计的下游（本文档随任务推进更新）：

| 下游模块 | 依赖的 core 能力 | 任务 |
|---|---|---|
| Scheduler / 定时器 | `MonotonicClock`、`TickClock`、`ITimerQueue` | TASK-004 |
| 网络层 | `Uuid`（连接 / 会话 ID）、`ConfigManager`（监听地址） | TASK-005+ |
| 实体 / 场景 | `Uuid`（玩家 / 实体 ID，建议 V7） | TASK-007+ |
| 逻辑层（玩法） | `CommandBus` / `QueryBus` / `EventBus`（跨模块调用骨架） | TASK-007+ |
| 战斗 / 技能 | `TickClock`（技能 CD 按 Tick 计） | TASK-012+ |
| 存档 / DB | `Uuid` V7 作主键、`WallClock`（落盘时间戳） | TASK-015+ |

## 五、禁止的依赖

- **禁止 core 依赖 Game / Gameplay / 任何业务模块**（依赖单向规则的硬性要求）。
- **禁止下游 `#include` core 的 `src/` 或内部头。** 以下四个头是模块私有实现，
  变更不受接口兼容性保护：
  - `engine/core/src/time/wall_clock_seam.h`（测试注入缝）
  - `engine/core/src/uuid/entropy.h`（OS 熵源封装）
  - `engine/core/src/config/json_parser.h`（JSON 解析）
  - `engine/core/src/config/config_snapshot.h`（不可变快照）
- **禁止 core 内部跨子模块访问私有数据。** 例如 config 不许去读 uuid 的
  `bytes` 成员以外的内部状态；time 不许改 config 的快照。
- **禁止引入会破坏「离线可绿」的依赖。** 当前 vcpkg 离线拉不到包，
  任何新增三方依赖都必须先验证本地能构建通过。
- **禁止 core 依赖日志模块。** core 的三件套（error / time / uuid / config）
  在失败时返回 `Error` 让调用方决定怎么记，不自己打日志 ——
  否则日志初始化前的早期失败会无路可走。

## 五之二、TASK-004 新增的线程归属红线

- **禁止 Scheduler 创建或使用执行线程。** `engine/core/{src/sched,include/mmo/core/sched}`
  下任何文件（含注释）出现 `std::thread` 字面量即验收失败。定时器必须由宿主线程驱动。
- **禁止 `ObjectPool` / `Arena` 跨线程共享。** 二者单线程拥有、热路径无锁，
  跨线程共用会静默数据竞争。需要跨线程复用内存请用 `MemoryPool`（它有有界跨线程归还队列）。
- **禁止 `Thread` 新增第五类角色。** `ThreadRole` 枚举值固定（Network/Simulation/Worker/
  Persistence），序列化稳定；新增必须走架构评审。
- **禁止 `Scheduler::Tick` 的 `now` 倒流。** 宿主必须传单调不减的时刻；
  倒流（多半是误用墙钟）会被 `Tick` 当场以 `INVALID_ARGUMENT` 拒绝，而不是悄悄接受。
- **禁止周期定时器 `period <= 0`。** 否则 `Tick` 的 catch-up 会无限触发，
  入口即拦下返回 `INVALID_ARGUMENT`。
- **禁止 `CatchUpSteps` / `kMaxCatchUpPerTick` 形同虚设**：周期定时器一次 Tick 内最多补
  `kMaxCatchUpPerTick=8` 次，命中限幅后 deadline 快进到 `now` 之后丢弃积压 ——
  否则「补触发 → 更慢 → 补更多」的死亡螺旋会拖垮整服。

## 五之三、TASK-007 新增的总线红线

- **禁止 EventBus 创建或使用执行线程。** `engine/core/src/bus` 下任何文件（含注释）
  出现 `std::thread` 字面量即验收失败；`Drain` 必须由宿主线程在 Tick 的 Event 阶段驱动。
- **禁止 `mmo_core_bus` 依赖 `protocol` / `rpc` 模块。** 总线只服务进程内逻辑线程；
  进程间一律走 TASK-005 Protocol + TASK-006 RPC。复用 TASK-002 ID 类型，
  保持「Core 是依赖图叶子」的单向规则。
- **禁止把 `engine/core/src/bus/*.cpp` 暴露给下游**：总线只暴露
  `include/mmo/core/bus/*.h`，`event_slot.h` 的类型擦除实现仅供总线内部使用。
- **禁止丢弃关键事件**（`TEvent::kCritical`）：队列满必须返回 `BUSY` 交还背压，
  非关键事件丢弃计数是降级路径，不是关键事件的出路。
- **禁止 Query 的 `Ask` 区间内产生任何写操作**（协作式约束，靠 `const` 入参 +
  `SideEffectProbe` 抓违规）。

## 六、接口兼容性

- 本模块的所有公开头在对应任务 `STATUS: DONE` 后**冻结**。
- 破坏性变更（改签名 / 删字段 / 改语义）必须走 `version` 字段 + 兼容性评估，
  禁止静默改签名导致下游编译失败。
- `Uuid::bytes` 是**公开成员**（TASK-003 §7 明确要求），下游可读可拷贝，
  但**禁止直接改写 version / variant 位** —— 那会产出非法 UUID。
- `ITimerQueue` 目前只有接口（TASK-003 定义，TASK-004 实现）。
  在 TASK-004 落地前，禁止任何模块自行实现「差不多的」定时器。
