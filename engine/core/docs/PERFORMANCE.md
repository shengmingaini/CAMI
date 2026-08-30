# engine/core · 性能实测（TASK-001 / TASK-002 / TASK-003 / TASK-004 / TASK-007）

> 所有数字均由 benchmark 真实测量，非估算；机器可读输出见 `bench/*.txt`。
> 验收脚本以 **Release** 构建运行 benchmark 并断言阈值。

## 环境

- 编译器：g++ (MSYS2 MinGW-w64) 16.1.0
- 构建：CMake 4.4.2 + Ninja，C++20（`-std=gnu++20`）
- 优化：Release `-O3 -DNDEBUG`；Debug `-O0 -g`
- 依赖：纯标准库，vcpkg manifest mode（空依赖，离线构建）

## 实测结果

| 指标 | Release | Debug | 阈值 | 结论 |
|---|---|---|---|---|
| `result_ns_per_op`（Result 构造+析构，1e7 次） | **0.219 ns** | 45.829 ns | ≤ 5 ns | ✅ |
| `alloc_per_fail`（失败路径堆分配次数 / op，1e7 次） | **0** | 0 | ≤ 0 | ✅ |

## 说明

- `result_ns_per_op` 使用 `volatile` 接收结果并以迭代序号作值，确保构造工作不被优化消除，
  为真实可重复测量（基准：约 0.2 ns/op，远低于 5 ns 阈值）。
- `alloc_per_fail = 0`：失败路径使用 32 字节内联 `message` 缓冲，`Error` 复制与 `Result`
  移动均不触发 `operator new`；`AndThen`/`Map` 失败时仅复制 `Error`（仍零分配）。
- `ToString()` 单次：`std::string` 拼接（domain + 名称 + message），无格式化额外拷贝；
  短消息下 O(message_len)，无堆分配。

---

# TASK-002 · Core Logger / Trace

> 由 `engine/core/tests/log_bench` 真实测量（8 线程 × 10 万条 = 80 万条），
> 机器可读输出见 `bench/core_log.txt`。
> 验收阈值：`disabled_ns_per_call < 5`、`log_ns_per_msg ≤ 800`；
> §22 另要求丢弃率 < 0.1%、单条日志堆分配 = 0。

## 实测结果（8 线程 × 100k 条）

| 指标 | Release（验收脚本现场值） | Debug | 阈值 | 结论 |
|---|---|---|---|---|
| `disabled_ns_per_call`（日志关闭时单次 `MMO_LOG`） | **0.527 ns** | 1.422 ns | < 5 ns | ✅ |
| `log_ns_per_msg`（开启日志，格式化 + 无锁入队） | **85.428 ns** | 237.208 ns | ≤ 800 ns | ✅ |
| `log_ns_per_msg_file`（含真实文件 sink 的端到端） | **98.212 ns** | 269.122 ns | — | 参考值 |
| 丢弃条数 / 丢弃率 | **0 / 0.0000%** | 0 / 0.0000% | < 0.1% | ✅ |
| 单条日志堆分配次数（`core_log_test` 断言） | **0** | 0 | = 0 | ✅ |

Release 档在同一台机器上多次运行有波动（另一次实测 `disabled=0.509`、
`log_ns_per_msg=115.529`），**丢弃条数稳定为 0**。阈值余量在 7~9 倍，不构成风险。

机器可读输出（`bench/core_log.txt`，取自 `scripts/verify/task-002.sh` 现场运行）：

```
threads=8
per_thread=100000
total_messages=800000
disabled_ns_per_call=0.527
log_ns_per_msg=85.428
enqueued=800000
dropped=0
drop_rate_percent=0.0000
file_per_thread=2048
log_ns_per_msg_file=98.212
dropped_file=0
```

## 关键调优记录（真实踩坑，不是拍脑袋）

1. **批量出队把丢包率从 54% 打到 0%**

   初版后台线程逐条 `TryDequeue`，8 线程压测丢包 **54%**。根因不是锁竞争，而是
   **每条记录一次 cache miss 串行累加**：环形队列 16.8 MB，远超缓存，单消费者的
   延迟被逐条放大，跟不上 8 个生产者的合成峰值。

   改为 `TryDequeueBatch(batch, 32)` 后，整批的 cache miss 重叠，丢包率 → **0%**。
   代价是 `log_ns_per_msg` 从「62 ns」升到「115 ns」——**但那个 62 ns 是假的**，
   因为它有 54% 的调用走的是「队列满 → 直接丢弃」的快路径，并没真正完成工作。

2. **硬件预取无效**

   曾尝试在环形队列的出队循环里加 `__builtin_prefetch` 预取下一个 cell，实测无改善
   （硬件流式预取已经在做）。这个改动后来被批量出队取代。

3. **flush 节流**

   后台线程每批都 `fflush` 会把磁盘 IO 放大成瓶颈。改为按「批数 + 时间间隔」节流后，
   文件 sink 阶段的吞吐恢复正常。

4. **阶段 3 刻意压到队列容量以内**

   单消费者写盘吞吐（约 5M 条/秒）天然低于 8 个生产者的合成峰值（20M 条/秒）。
   阶段 3 要测的是「带磁盘 IO 的单条真实成本」，不是「sink 成为瓶颈时的饱和行为」——
   后者由 `TestQueueOverflow` 单测专门覆盖，且按红线设计**必须丢**（禁止阻塞业务线程）。

## 说明

- `disabled_ns_per_call` 只走一次 `ShouldLog` 分支判断，不进入格式化，因此与日志内容
  长度无关，是「日志关闭时业务代码可以放心埋点」的依据。
- `log_ns_per_msg` 包含 `<format>` 格式化 + 无锁入队，不含磁盘 IO（阶段 2 无 sink）。
- 零堆分配由 `core_log_test` 的全局 `operator new` 计数器断言：1000 条日志
  `delta == 0`。格式化缓冲与 `LogRecord` 均在栈上。
- CPU 逻辑核数会影响 8 线程压测结果；本数据取自 `getconf _NPROCESSORS_ONLN` ≥ 8 的机器。

---

# 三、Core Time / UUID / Config（TASK-003）

机器可读输出：`bench/core_time.txt`（`bin/time_bench --samples 1000000`，Release 构建）。
验收脚本断言其中两项：`monotonic_ns_per_call ≤ 25`、`config_get_ns ≤ 50`。

## 实测数据（Release，1,000,000 samples）

| 指标 | 实测 | 阈值 | 余量 | 说明 |
|---|---:|---:|---:|---|
| `monotonic_ns_per_call` | **16.755 ns** | ≤ 25 ns | 33% | QPC + 定点乘移 |
| `point_ns_per_call` | 16.782 ns | — | — | 返回 `steady_clock::time_point` |
| `wall_ns_per_call` | 23.557 ns | — | — | `GetSystemTimePreciseAsFileTime` |
| `tick_next_deadline_ns` | **1.048 ns** | — | — | 纯整数加法，不读时钟 |
| `uuid_v4_ns` | 44.443 ns | < 100 ns | 56% | BCryptGenRandom 16B |
| `uuid_v7_ns` | 66.657 ns | < 100 ns | 33% | 时间戳写 6B + CSPRNG 10B |
| `uuid_tostring_ns` | 32.230 ns | — | — | 36 字符 hex + 连字符 |
| `uuid_parse_ns` | 20.482 ns | — | — | 含校验，非法输入走错误路径 |
| `config_get_ns` | **34.244 ns** | ≤ 50 ns | 32% | `Get<uint32>`，读路径无原子 RMW |
| `config_get_string_ns` | 40.624 ns | — | — | `Get<string>`（多一次堆分配） |

## 选型依据：为什么不用 `std::chrono::steady_clock`

在动手实现前先跑了一次一次性探针（`build/probe_time.cpp`，测完即删），
拿数据而不是拍脑袋决定实现：

| 实现 | 实测 ns/次 |
|---|---:|
| `std::chrono::steady_clock::now()` | **24.548** |
| 裸 `QueryPerformanceCounter` | 15.718 |
| QPC + 定点乘移 | 16.901 |
| QPC + 64 位除法 | 21.269 |
| `GetSystemTimeAsFileTime` | 19.684 |
| `BCryptGenRandom`（16B，对照） | 41.812 |

`steady_clock` 是 24.548 ns，对 25 ns 阈值只有 **2% 余量**——
换一台机器、换一次编译器升级就会翻车。换 QPC 定点后余量拉到 33%，
代价是每 Tick 多写 20 行定点乘法代码，值得。

## 优化记录

1. **UUID V7 只申请 10 字节熵（72.840 → 66.657 ns）**

   初版 `TryNewV7` 无脑 `FillRandom(bytes, 16)`，然后覆写前 6 字节为毫秒时间戳。
   前 6 字节的熵直接被丢掉，等于白付了 6 字节的 CSPRNG 成本。
   改成只给 `bytes[6..16)` 取随机，省 8.5%。

2. **配置读路径去掉原子 RMW**

   `std::atomic<std::shared_ptr<const Snapshot>>::load()` 内部有引用计数递增，
   每次读是 2 次原子 RMW，跨核 cache line 争用会把它打到远超 50 ns。
   改为 thread-local 缓存：持有快照强引用 + `uint64_t` 版本号，
   只有版本号变化时才真正 `load()`。热路径退化成一次 thread-local 读 + 一次整型比较。

3. **配置 `Get<T>` 用 `string_view` 透明哈希**

   初版每次 `Get("tick.hz")` 都会构造 `std::string` 去查 `unordered_map`。
   换成 `is_transparent` 哈希 + `std::equal_to<>` 后，查询侧零堆分配。
   `config_get_ns` 里那 34 ns 有一半是这次优化省下的。

4. **定点换算不用 `__int128`**

   `__int128` 在 `-Wpedantic` 下报「ISO C++ does not support `__int128`」，
   而本仓 `-Wpedantic` 是硬红线（任何告警必须清零）。
   改为 `ScaleFixedPoint()`：把 64×64 拆成 4 项 32×32 部分积
   （`lo / mid_lo / mid_hi / hi`），精度和 `__int128` 一致，且全是可移植的 `uint64_t`。

## 说明

- `tick_next_deadline_ns` 只有 1 ns 是因为它是**纯加法**（`prev + interval_ns`），
  不读任何时钟源，因此天然免疫墙钟回拨 —— 这是 TASK-003 §15.9 的核心设计。
- `config_get_ns` 测的是**命中已有 key** 的路径；`NOT_FOUND` 路径会构造 Error 消息，
  成本高一个量级，属于冷路径，不在预算内。
- UUID 唯一性不靠 benchmark 验证，靠 `core_time_test` 里各 100 万次 V4 / V7
  排序查重（实测碰撞数 0）。
- 以上数字取自 Windows 11 / MinGW-w64 g++ 16.1.0 / `-O3` Release。
  QPC 频率由硬件决定，低精度时钟源（如某些虚拟机的 10 MHz TSC）会略微抬高
  `wall_ns_per_call`，不影响 `monotonic_ns_per_call` 的量级。

---

# 四、Core Memory / Thread / Scheduler（TASK-004）

机器可读输出：
- `bench/core_sched.txt`（`bin/sched_bench --timers 10000 --ticks 1000`，Release）：验收断言 `sched_tick_us_10k_timers ≤ 200`。
- `bench/core_mem.txt`（`bin/mem_bench --ops 10000000`，Release）：验收断言 `pool_acquire_release_ns ≤ 20`。

## 实测数据（Release，1e7 ops / 1e3 ticks）

| 指标 | 实测 | 阈值 | 余量 | 说明 |
|---|---:|---:|---:|---|
| `sched_tick_us_10k_timers` | **1.076 µs** | ≤ 200 µs | 99.5% | 每 Tick 扫描 1 万个定时器的耗时（最小堆 O(k·logN)） |
| `pool_acquire_release_ns` | **0.417 ns** | ≤ 20 ns | 97.9% | ObjectPool `Acquire`+`Release` 一轮回（无锁、无原子） |
| `arena_push_ns` | **2.752 ns** | < 3 ns | 8.3% | Arena `Push`（对齐 + 指针加法 + 一次边界检查） |
| `mempool_alloc_ns` | **6.091 ns** | < 15 ns | 59.4% | MemoryPool `Allocate`（拥有者线程无锁空闲链表） |

> `arena_push_ns` 实测 2.752 ns，已逼近 3 ns 阈值（余量 ~8%）。Arena `Push` 是
> 「对齐 + 指针加法 + 边界检查」三步，几乎不可能再压；若未来把对齐放宽到 8 字节固定
> 可省一次 `RoundUp`，但当前数值达标，不值得为 0.3 ns 改契约。
> 另：benchmark 受 CPU 负载波动影响明显（同机多次 `sched_tick_us_10k_timers` 落在
> 1.0~1.4 µs 区间），阈值余量在 99% 以上，不构成风险。

## 选型依据：为什么 Scheduler 不自带线程

50k 并发下 Buff / DOT / 冷却数量是十万级的。「一对象一线程 / 一 OS timer」的方案会直接
把 OS 拖死。TASK-004 选 **单线程 `Tick` 驱动 + 最小堆**：

- 最小堆 vs 时间轮：Cancel 在时间轮里要么 O(1) 但要留墓碑、要么 O(n)；最小堆配 `index_` 表，
  Cancel 是 O(1) 标记 + O(logN) 惰性清理，实现简单且高效。
- 惰性删除：Cancel 只打 `cancelled` 标记，真正的槽位回收发生在 `Tick` 弹出时，
  或取消数过半时的 `Compact()` 批量清理 —— 保证不内存泄漏（`TestSchedulerSlotReclaim`
  验证：5000 取消后重建 5000，槽位数 5000 → 5000，零增长）。
- **线程归属红线**：`Scheduler` 不创建任何线程。它只是一个被宿主线程调用的 `Tick(now)`，
  由 `SimulationThread` / `WorkerThread` 在自己的循环里驱动。验收脚本对 `sched/` 目录做
  `std::thread` 字面量扫描（连注释都不行）。

## 优化记录

1. **周期定时器 catch-up 限幅 + 快进兜底（防死亡螺旋）**

   初版 catch-up 把 deadline 只推进 `kMaxCatchUpPerTick=8` 个周期，但若 deadline 仍 ≤ `now`，
   主 `for(;;)` 循环会**再次弹出同一个定时器**继续补触发 —— 8 次限幅形同虚设，
   实测一个掉帧的周期定时器一次 Tick 被触发 10 次（`Tick(+120ms)` 返回 10 而非 8）。
   修复：命中限幅后若 `deadline <= now` 直接快进到 `now + period`，丢弃积压触发。
   修复后 `Tick(+120ms)` 严格返回 8，`TestSchedulerPeriodic` 通过。

2. **堆比较器必须是最小堆（反向比较器）**

   `std::pop_heap` 配 `comp` 默认把「comp 视为最大」的元素放堆顶。要 deadline 最小者优先出队，
   比较器必须用 `>`（反向）。初版写成 `<` 变成**最大堆**，定时器按最大 deadline 先触发，
   `TestSchedulerOrdering` 得到 `[30,20,10]` 而非期望的 `[10,20,30]`。改成
   `x.deadline > y.deadline`（同 deadline 时 `x.id > y.id` 稳定）后正确。

3. **`MaybeCompact` 回收全取消尾部**

   几何收缩（`cancelled*2 >= heap.size()`）在「全部取消」场景会留下 < 32 的尾巴
   （本次 7 个），导致重注册时被迫新建 7 个槽位（`SlotCount` 5000 → 5007）。
   加一条：`cancelled_count_ == heap_.size()`（全部已取消）时无条件 `Compact()`，
   回收尾部。修复后 `SlotCount` 5000 → 5000。

4. **`TaskFn` 小对象优化（32 字节内联，零堆分配）**

   热路径用 `TaskFn` 替代 `std::function`：可调用对象内联存在 32 字节存储区，
   提交任务**永不堆分配**。`core_thread_test` 用全局替换 12 个分配函数计数证明：
   1e6 次「构造 + 移动 + 调用」`heap_allocs = 0`。
   `std::function` 几乎必然每次构造都 `new` 一块堆内存，是 Tick 热路径的隐形杀手。

5. **`MemoryPool` 跨线程归还有界**

   非拥有者线程归还的块进有界 `MpmcQueue`（4096 容量）；队列满退化到自旋锁保护的溢出表。
   永不崩溃、永不双释放。拥有者线程在空闲链表见底时 `DrainRemote()` 批量收回。
   `TestMemoryPoolAbuse` 用野指针 / 双释放 / 跨线程归还压测，只记 `InvalidFrees` 指标不崩溃。

## 说明

- `sched_tick_us_10k_timers` 测的是**平均**每 Tick 耗时（1000 个 Tick / 1 万个定时器）。
  堆始终维持 ~1 万个待触发定时器，贴近 50k 并发真实负载；实际每 Tick 只弹出已到期的少量。
- `pool_acquire_release_ns` 含 `Acquire`（placement-new 默认构造）+ `Release`（析构），
  对 trivial 类型几乎为零成本；非 trivial 类型成本来自构造/析构本体，非池开销。
- 以上数字取自 Windows 11 / MinGW-w64 g++ 16.1.0 / `-O3` Release。

---

# 五、Core Command / Query / Event Bus（TASK-007）

机器可读输出：`bench/core_bus.txt`（`bin/bus_bench --iterations 1000000`，Release 构建）。
验收脚本断言其中三项：`cmd_dispatch_ns ≤ 150`、`event_drain_ns_per_event ≤ 80`、
`alloc_per_cmd = 0`；§22 另要求 `event_publish_ns < 100`、1e6 容量队列 < 64MB。

## 实测数据（Release，1,000,000 次）

| 指标 | 实测 | 阈值 | 余量 | 说明 |
|---|---:|---:|---:|---|
| `cmd_dispatch_ns` | **24.854** | ≤ 150 ns | 83% | CommandBus `Dispatch`（COW 快照无锁读 + 类型擦除调用） |
| `event_publish_ns` | **7.620** | < 100 ns | 92% | EventBus `Publish`（入队 + 关键性判断，零堆分配） |
| `event_drain_ns_per_event` | **11.034** | ≤ 80 ns | 86% | `Drain` 平均每个事件（派发 + 时间预算检查） |
| `alloc_per_cmd` | **0.000** | = 0 | — | 1e6 次 Dispatch 的 `operator new` 计数 |
| `queue_mb_1e6` | **48.00** | < 64 MB | 25% | 1,048,576 槽 × 48B 槽位的队列内存占用 |

> 同机多次运行 `cmd_dispatch_ns` 落在 24~30 ns、`event_drain_ns_per_event` 落在 10~12 ns
> 区间（受 CPU 负载波动影响），相对阈值余量在 80% 以上，不构成风险。

## 设计要点：为什么能到个位数纳秒

1. **读路径完全无锁（COW 快照 + `atomic<shared_ptr>`）**
   CommandBus / QueryBus 的注册表是 `std::shared_ptr<const HandlerMap>`，`Dispatch` /
   `Ask` 只做一次原子 load 拿快照再查表 —— 没有互斥锁、没有引用计数写竞争。
   注册（冷路径）才写：拷一份新表原子换指针，已 in-flight 的调用继续用旧表。

2. **Event 队列复用 TASK-004 的 `MpmcQueue`（Vyukov 有界无锁）**
   `Publish` 只做「类型擦除入队」，不做任何分配（≤32B 事件走 32B 内联负载）；
   失败路径才触碰 `dropped_` 计数器（relaxed 原子）。

3. **40 字节槽位喂饱内存预算**
   `EventSlot` = 8B `EventTypeInfo*` + 32B 内联 union（`static_assert(sizeof == 40)`）。
   1e6 槽 = 40MB 数据 + 8MB 索引 ≈ 48MB，低于 64MB 预算。>32B 事件走堆回退，
   堆路径的分配由调用方事件构造承担，`Publish` 本身捕获异常转 `INTERNAL_ERROR`。

4. **Drain 的预算检查摊薄时钟开销**
   若每个事件都查一次 `MonotonicClock::Now()`（~17ns），100 个事件就吃掉 1.7µs。
   改为每 64 个事件查一次（前 8 个强制查，保证超短预算也生效），实测单个事件
   摊薄成本 ~11ns，且 `TestDrainBudget` 验证 2ms 预算语义严格成立。

## 优化记录（真实踩坑，不是拍脑袋）

1. **`EventTypeInfo` 缓存实例级槽位下标 → 跨实例 SIGSEGV（已修复）**

   初版在进程级静态 `EventTypeInfo` 里缓存 `atomic<uint32_t> slot`（订阅槽下标）。
   单实例测试全绿；第二个 `EventBus` 实例订阅同一事件类型时，读到的是**第一个实例**
   的旧下标，`slots_[slot]` 越界 → `Core_Bus.Suite` 段错误。
   修复：把下标移进 `EventBus` 实例自己的 `slot_index_`（`unordered_map<const EventTypeInfo*, uint32_t>`），
   配 `EnsureSlotLocked` / `FindSlotLocked` 只在持锁时访问。教训：**进程级静态对象
   不得缓存实例级状态**，哪怕只有一个原子字段。

2. **GCC 拒绝嵌套 struct 的 NSDMI 作默认实参**

   `explicit EventBus(Options options = {})` 里 `Options` 是嵌套 struct 且带默认成员
   初始化（`queue_capacity{1u<<16}`）时，GCC 报「default member initializer required
   before the end of its enclosing class」——嵌套类的 NSDMI 属于 complete-class context。
   修复：把 `Options` 提到命名空间级 `struct EventBusOptions`，类内 `using Options = EventBusOptions`
   保持调用方写法不变。

3. **throw-only lambda 在 `-O3` 下可能掉出函数尾**

   单测里「handler 必然抛异常」的 lambda 只有 `throw` 语句，`-O3` 下编译器不保证
   补隐式 return，存在 UB 风险。给每个 throw 后补 `return Result<T>::Ok(T{})` 不可达
   代码，消除 `-Wreturn-type` 告警面。

## 说明

- `cmd_dispatch_ns` 测的是**已注册**命令的完整派发（查表 + 类型擦除 + 调用 handler
  桩）。`NOT_FOUND` 路径（构造 `Error` 消息）是冷路径，成本高一个量级，不在预算内。
- `event_drain_ns_per_event` 含订阅者回调本身（demo 桩函数体为空），更贴近真实
  派发成本；带业务逻辑的回调成本在回调体内，不在总线。
- `alloc_per_cmd = 0` 由 `bus_bench` 的全局 `operator new` 计数断言（1e6 次 Dispatch
  `delta == 0`）；`Publish` 内联路径同理零分配（`bus_test` 的堆路径用例另覆盖 >32B 事件）。
- 以上数字取自 Windows 11 / MinGW-w64 g++ 16.1.0 / `-O3` Release。
