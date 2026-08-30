# engine/core · 测试说明（TASK-002 / TASK-003 / TASK-004 / TASK-007）

> TASK-001（Error/Result）的测试在 `tests/error_test.cpp`，风格与本文件一致。
> TASK-003（Time / UUID / Config）的测试在 `tests/time_test.cpp`，见本文件末尾第三部分。
> TASK-007（Command / Query / Event Bus）的测试在 `tests/bus_test.cpp` + `demo_pipeline.cpp`，见本文件末尾第五部分。
> 全部测试共用 `tests/test_print.h` 作为输出通道（红线禁止 `printf` / `cout` / `cerr`）。

## 为什么不用 GoogleTest

vcpkg 在当前环境下**离线不可用**（GFW + baseline `aae277ac` 空依赖），拉不到 gtest。
因此测试采用自包含 harness：

- `CHECK(cond)` 宏 + 计数 `failures`，`main` 返回非 0 即 `ctest` 判失败；
- 全局 `operator new` / `operator delete` 计数器，用于「零堆分配」这类硬断言；
- 无第三方依赖，`configure` 一次即可跑，构建链路最短。

## 测试目标与入口

| 目标 | 命令 | 说明 |
|---|---|---|
| 全量单测 + 集成 + 失败测试 | `ctest -R Core_Log` | 含格式串反向编译校验 |
| 直接跑（CWD = 仓库根） | `./build/Release/bin/core_log_test.exe` | 会落盘 `bench/trace_probe.log` |
| 压测 | `./build/Release/bin/log_bench.exe --threads 8 --per-thread 100000` | 写 `bench/core_log.txt` |
| 按 TraceID 串联 | `python tools/logtrace/parse_trace.py <trace> bench/trace_probe.log` | 见 README |

> 注意：`core_log_test` 必须在**仓库根**运行，`bench/trace_probe.log` 才会落对位置
> （`ctest` 的 CWD 是构建目录，此时会落到 `build/<cfg>/bench/`，不影响测试判定）。

## 覆盖清单

### §16 单元测试

| 用例 | 断言要点 |
|---|---|
| `TestLogLevel` | 6 个级别 `ToString` → `ParseLogLevel` 往返一致；大小写不敏感与别名（`WARNING`/`Err`/`critical`）；非法串返回 `nullopt`；`"INFO "` 这类带空格的显示名不是合法配置名 |
| `TestTraceId` | `SetNodeId` 生效且 `NodeOf` 可反查；**100 万次 `NewTraceID` 严格递增**（递增即唯一）；`DeriveRequestID` 两次结果互不相同，但低 48 位与来源 Trace 一致（可反查所属请求） |
| `TestLogContext` | 初始为空；`ScopedLogContext` 嵌套时内层只覆盖指定字段、其余继承外层；内层析构后恢复外层，外层析构后回到空；`WithContext` 整体替换并自动恢复 |
| `TestRingBuffer` | SPSC 下 1000 条出队顺序严格递增；容量 8 的队列第 9 条**入队失败而非阻塞**；出队一条后槽位归还、可再次入队 |
| `TestFormatter` | 时间戳（epoch / 跨天 / 跨年 / 闰年 1972-02-29）；缓冲不足返回 0 不溢出；文本行九字段 key 齐全；JSON 行 10 个 key 齐全；JSON 转义（引号 / 换行 / 反斜杠）；未设置的 player/scene 在文本模式为 `-`、JSON 为 `null`；超长 message 截断且不越界 |
| `Core_Log.FormatStringNegative`（ctest 用例） | 反向编译校验：见下节 |

### §17 集成测试

| 用例 | 断言要点 |
|---|---|
| `TestTracePropagationAndFile` | 主线程设 TraceID → `WithContext` 携带到 worker → 子线程打日志；捕获到 3 条，`trace_id`/`player_id`/`scene_id`/`service`/`module` 全部一致；两条 worker 日志 `thread_id` 相同且与主线程不同；时间戳不倒退；落盘文件中同一 `trace=` 恰好 3 行；未初始化时写入静默丢弃不崩溃 |
| `TestJsonMode` | JSON 模式下 10 个 key 齐全，`level`/`service` 取值正确，message 正确嵌入 |
| `TestRotation` | 滚动后 `app.log`/`.1`/`.2`/`.3` 存在，`.4` 不存在（超出保留份数被删）；当前文件大小 ≤ 阈值 |
| `TestTraceProbe` | 落盘探针：写 `bench/trace_probe.log` 共 4 条同 Trace 日志（主线程 2 条 + worker 2 条），打印 TraceID 供 `parse_trace.py` 真实检索 |

### §19 失败测试

| 用例 | 断言要点 |
|---|---|
| `TestQueueOverflow` | 队列容量故意开到 256；后台线程首条记录阻塞 200 ms 模拟卡死；4×5000 条业务写入在 3 s 内返回（**不阻塞**）；`dropped > 0` 且仍有日志成功入队；溢出告警 ≥ 1 条且 ≤ 4 条（按突发收敛，不刷屏） |
| `TestInitFailure` | 日志目录不存在时 `Init` 返回 `Error`（`INTERNAL_ERROR`，message 含 `open log file failed`）而非崩溃；失败后 `MMO_LOG` 静默丢弃；未初始化时 `Shutdown` 是 no-op；**重复 `Init` 返回 `BUSY`** 而不是偷偷重启线程 |
| `TestFatalFlush` | `MMO_LOG_FATAL` 后**不调用 `Logger::Flush()`**，文件内容已包含该行（Fatal 自己保证落盘） |

### §20 验收项

| 用例 | 断言要点 |
|---|---|
| `TestDisabledCost` | 级别设 Fatal 后 `ShouldLog(Info) == false`、`ShouldLog(Fatal) == true`；100 万次 `MMO_LOG` 关闭路径 < 1000 ns/次（精确阈值由 `bench/core_log.txt` 断言 < 5 ns） |
| `TestZeroAllocation` | 预热后 1000 条日志的全局 `operator new` 增量 == 0 |

## 格式串编译期校验（反向编译测试）

§16 要求「格式化非法格式串时编译期报错」。这是**负向测试**——期望的是编译失败，
常规 `add_test` 无法表达，因此用 CMake 脚本驱动：

- 源码：`tests/log_format_fail.cpp`，里面故意写 `MMO_LOG_INFO("a={} b={}", 1);`
  （占位符 2 个、实参 1 个）。
- 驱动：`cmake/NegativeCompileCheck.cmake`，在构建期尝试编译该文件，
  **编译成功才算测试失败**。
- 前提：本工具链的 libstdc++ 已支持 `<format>` 的 `consteval` 格式串校验
  （已实测：参数不足时 `g++ -std=c++20` 直接报错）。

## 手工复核清单（脚本无法自动判定，提交前逐条勾选）

- [ ] 九项字段全部出现在每条日志记录中（JSON 模式 key 齐全，单测已断言）
- [ ] 8 线程 × 10 万条压测无死锁、无交叉错乱，单线程内顺序严格递增
- [ ] `python tools/logtrace/parse_trace.py <trace>` 能完整串起跨线程日志
      → 跑 `./build/Release/bin/core_log_test.exe`，用 stderr 里的
      `trace_probe: file=... trace=...` 取 TraceID 再执行
- [ ] 关闭日志时单次 `MMO_LOG` 开销 < 5 ns（见 `bench/core_log.txt`）
- [ ] 环形队列满时 `dropped` 计数正确且业务线程不阻塞
- [ ] 全仓 grep 无 `std::cout` / `printf` 直接输出
- [ ] Debug / Release 双构建通过，`ctest -R Core_Log` 全绿

---

# 三、Core Time / UUID / Config（TASK-003 · `tests/time_test.cpp`）

一个可执行文件覆盖 §16 单测 / §17 集成 / §19 Failure 三类，ctest 用例名 `Core_Time.Suite`。
**ctest 必须带 `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}`** —— 配置测试要读相对路径 `config/`，
而 ctest 默认工作目录是 build 目录，不设会全部 `NOT_FOUND`。

## §16 单元测试

| 用例 | 断言要点 |
|---|---|
| `TestMonotonic` | 100 万次 `MonotonicClock::Now()`，**回退次数 == 0** |
| `TestWallClock` | `UnixMillis()` 落在合理区间（2001-2100），`UnixNanos()` 与 millis 自洽 |
| `TestTickClock` | 20 Hz → `TickIntervalNs() == 50'000'000`；`CatchUpSteps` 在超长间隔下被限幅到 3；间隔不足时返回 0 |
| `TestUuidFormat` | V4 version=4 / variant=2；V7 version=7；`ToString` 长度 36、连字符位置正确；`Parse` 往返一致；非法输入 → `INVALID_ARGUMENT` |
| `TestUuidUniqueness` | 各生成 **100 万**个 V4 / V7，排序后查重，**碰撞数 == 0** |
| `TestUuidV7Ordering` | 同一毫秒内 / 跨毫秒生成的 V7 序列，`TimestampMillis()` 单调不减；V7 可按字符串排序 |
| `TestConfigBasics` | `LoadDir("config")` 后 `Size() == 10`；`Get<uint32>("tick.hz") == 20`；`Get<string>("service.name") == "gamenode"`；缺失 key → `NOT_FOUND` 且消息含 key 名；类型不匹配 → `INVALID_ARGUMENT`；`Version()` 单调递增 |

## §17 集成测试

| 用例 | 断言要点 |
|---|---|
| `TestFakeTickLoop` | 20 Hz 驱动假 Tick 循环跑 **10 秒**，实测 Tick 数 `199`（要求 200 ± 2），**累计漂移 0 ns** |
| `TestConfigHotReloadConcurrency` | 4 个读线程持续 `Get<uint32>("probe.hz")`，写线程在 20 / 30 之间来回热更；读满 100 万次后停止，读到的值**只能是 20 或 30**，异常值 **0**（实测 1,057,587 次读） |

## §19 Failure 测试

| 用例 | 断言要点 |
|---|---|
| `TestWallClockRollback` | 用 `time_internal::SetInjectedWallClockNanos` 注入 −5 秒回拨、再注入 +60 秒跳变；期间 200 次 Tick 的 deadline 序列**仍严格单调递增**（证明 Tick 不依赖墙钟） |
| `TestUuidEntropyFailure` | 用 `uuid_internal::SetTestFillRandom` 注入失败的熵源；`TryNewV4/V7` → `INTERNAL_ERROR`，`NewV4/V7` → `Nil()`，**绝不产出弱随机 UUID** |
| `TestConfigFailure` | ① 非法 JSON → `INVALID_ARGUMENT` 且旧快照不变；② 文件不存在 → `NOT_FOUND`；③ 目录不存在 → `NOT_FOUND`；④ 重复 key → `INVALID_ARGUMENT`；⑤ 热更时目录被删 → `Reload()` 报错且旧快照继续可用 |

## 为什么需要墙钟注入缝

§15.9 要求「证明墙钟回拨时 TickClock 不受影响」，但**测试进程无法真的改系统时间**
（改了会污染整台机器，且需要管理员权限）。所以在 `src/time/wall_clock_seam.h` 里留了一个
**内部注入缝** `SetInjectedWallClockNanos()`：非 0 时覆盖 `WallClock::UnixNanos()`。

它是 `src/` 下的内部头，下游 `#include` 不到，因此不存在「生产代码误调用」的风险；
生产路径上那个原子变量恒为 0，只多一次 relaxed load。

## 踩坑：`Result::Value()` 会抛异常

自研 `Result<T>` 的 `Value()` 内部是 `std::get`，对**错误结果**调用会抛
`std::bad_variant_access`。测试里一旦有 `ConfigManager::Get<T>(...).Value()` 的裸调用，
失败时你只能看到：

```
terminate called after throwing an instance of 'std::bad_variant_access'
```

——**完全不知道是哪一行断言挂了**。第一次跑验收脚本时就被这个坑了，
真正的失败原因（`LoadDir("config")` 因 ctest 工作目录不对而 `NOT_FOUND`）
被这层崩溃完全掩盖。

所以测试里统一用 `EXPECT_OK()` 宏包一层：

```cpp
template <typename T>
T CheckOk(const Result<T>& result, const char* expr, const char* file, int line) {
    if (!result.HasValue()) {
        ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s -> %s\n", file, line, expr,
                                    std::string(result.Err().Message()).c_str());
        ++g_failures;
        return T{};
    }
    return result.Value();
}
#define EXPECT_OK(expr) CheckOk((expr), #expr, __FILE__, __LINE__)
```

失败时打印 `FAIL @ time_test.cpp:519 : ConfigManager::Get<uint32>("tick.hz") -> key not found: tick.hz`。
**所有 `Result` 取值一律走 `EXPECT_OK`，禁止裸 `.Value()`。**

> 注意：`Result` 的 `using` 声明必须在 `CheckOk` 之前。C++ 对非依赖名的查找发生在
> 模板定义点，把 `using mmo::core::Result;` 放在后面的话，GCC 会报
> 「no matching function for call to CheckOk」这种完全指不到病根的错误。

## 手工复核清单（脚本无法自动判定，提交前逐条勾选）

- [x] 全仓 grep：Tick 相关路径不存在 `WallClock` / `std::chrono::system_clock`
- [x] TickClock 跑 10000 次，累计误差 **0 ns**（阈值 < 10 ms）
- [x] UUID 各 100 万次 V4 / V7 无碰撞，V7 可按时间排序
- [x] 配置热更期间 4 线程并发读，105 万次读无异常
- [x] benchmark 三项指标达标（16.755 / 44.443 / 66.657 / 34.244 ns）
- [x] `config/` 下 3 份配置正确加载，共 10 个 key
- [x] Debug / Release 双构建通过，`ctest -R Core_Time` 全绿

---

# 四、Core Memory / Thread / Scheduler（TASK-004 · `tests/thread_test.cpp`）

一个可执行文件覆盖 §16 单测 / §17 集成 / §19 Failure 三类，ctest 用例名 `Core_Thread.Suite`。
**ctest 必须带 `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}`** —— 同 TASK-003，配置测试要读相对路径 `config/`，
不设会全部 `NOT_FOUND`。三个子库 `mmo::core_thread` / `mmo::core_sched` / `mmo::core_memory`
均只依赖 `mmo::core_error` / `mmo::core_time`，红线禁止 `printf` / `cout` / `cerr`，统一走 `test_print.h`。

## 测试目标与入口

| 目标 | 命令 | 说明 |
|---|---|---|
| 全量单测 + 集成 + 失败测试 | `ctest -R Core_Thread` | 含 10s 并发长跑 |
| 直接跑（CWD = 仓库根） | `./build/Release/bin/core_thread_test.exe` | 落盘无，纯断言 |
| 调度器压测 | `./build/Release/bin/sched_bench.exe --timers 10000 --ticks 1000` | 写 `bench/core_sched.txt` |
| 内存池压测 | `./build/Release/bin/mem_bench.exe --ops 10000000` | 写 `bench/core_mem.txt` |

## §16 单元测试

| 用例 | 断言要点 |
|---|---|
| `TestTaskFn` | 32B 内联存储、`sizeof(TaskFn)==64`；noexcept 构造/移动；仅可移动（不可拷贝）；全局 `operator new` 替换证明热路径 `heap_allocs==0`；捕获 ≤32B 可正常调用 |
| `TestMpmcQueue` | Vyukov 有界无锁；4 生产者 ×4 消费者 ×10 万，`received==400000`、`duplicates==0`（无丢失、无重复）；满时 `TryPush` 返回 false（调用方转 `BUSY`） |
| `TestThreadLifecycle` | `Create` → `Post` → `PostBlocking` → `RequestStop` → `Join`；停止时**排空队列再退出**（优雅退出，不丢已入队任务） |
| `TestThreadBusy` | 队列压满后 `Post` 返回 `ErrorCode::BUSY` 而非阻塞 |
| `TestSchedulerOnce` | `ScheduleAfter` 一次性定时器到点触发恰好 1 次 |
| `TestSchedulerPeriodic` | `ScheduleEvery` 20Hz 长跑触发速率正确；`kMaxCatchUpPerTick=8` 限幅生效（见 §19 死亡螺旋防护） |
| `TestSchedulerCancel` | `Cancel` 幂等（重复取消不崩、不影响其它定时器） |
| `TestSchedulerCancelSelf` | 回调内调用 `Cancel(this)` 自身后不再触发 |
| `TestSchedulerOrdering` | 多 deadline 严格按 `[10,20,30]` 顺序触发（min-heap 校验，见 §踩坑） |
| `TestObjectPool` | `Acquire`/`Release`；`Release` 调析构（alive 递减）；池析构 `DestroyLiveObjects`（alive 归零）；单线程归属 |
| `TestMemoryPool` | `Allocate`/`Deallocate`；magic-number 守卫拦截 double-free / 野指针（`InvalidFrees` 计数）；跨线程归还走有界 `MpmcQueue` |
| `TestArena` | `Push(bytes,align)` 帧 bump 分配；`Reset()` 回收全部（指标归零） |

## §17 集成测试

| 用例 | 断言要点 |
|---|---|
| `TestIntegrationConcurrent` | 4 线程流水线（network → worker → sim → persist）跑 **10 秒**，posted≈13.5M、四阶段计数一致、`errors==0`；并行叠加 500×20Hz 调度突发 10s：500 定时器 / 10.0s / fires=100000（速率≈9990/s、误差≈0.1%）/ `max_ready==204`。证明 thread + sched + memory 三件套在并发下无丢失、无交叉错乱、无死锁 |

## §19 Failure 测试

| 用例 | 断言要点 |
|---|---|
| `TestSchedulerBackwards` | 传入回拨（过去）deadline：`Tick` 当帧补触发而非死循环 |
| `TestThrowingTask` | 任务回调抛异常被捕获，线程不崩、后续任务继续 |
| `TestSchedulerSlotReclaim` | 5000 次「注册→全取消→再注册」后 `SlotCount==5000`（几何 `MaybeCompact` 收敛，无槽位泄漏，见 §踩坑） |
| `TestMemoryPoolAbuse` | 重复 `Deallocate` / 野指针 `Deallocate` 被 magic 守卫拦截，`InvalidFrees` 递增、不崩溃 |

## 踩坑一：min-heap 比较器方向

`std::pop_heap` 把「比较器返回最大」的元素放到堆顶。要 min-heap（最早 deadline 在顶），
`HeapLess` 必须用**反向**（`>`）比较器：

```cpp
bool operator()(std::size_t a, std::size_t b) const noexcept {
    const Timer& x = (*slots)[a];
    const Timer& y = (*slots)[b];
    if (x.deadline != y.deadline) return x.deadline > y.deadline;  // 反向 = min-heap
    return x.id > y.id;
}
```

用 `<` 会变成 max-heap，`TestSchedulerOrdering` 得到 `[30,20,10]` 而非 `[10,20,30]`。

## 踩坑二：周期定时器 catch-up 死亡螺旋

`Tick` 在 `deadline <= now` 时按 `period` 推进 deadline；但当落后超过 `kMaxCatchUpPerTick=8`
轮后仍 `<= now`，外层 `for(;;)` 会**再次 pop 同一个定时器**继续补火，导致单帧触发数膨胀
（`Tick(+120ms)` 实测返回 10 而非 8）。修复：clamp 循环后若仍 `<= now`，直接
`deadline = now + period_ns` 丢弃积压，杜绝复利式补火。

## 踩坑三：几何 Compact 的尾部残留

`MaybeCompact` 原阈值 `cancelled*2 >= heap` 在「全取消后几何收缩」会留下 <32 的尾段，
导致再注册被迫开 7 个新槽（`SlotCount` 5007 > 5000）。补一条：
`cancelled_count == heap_.size()` 时**无条件** `Compact()`，尾部 7 槽被回收，5000→5000。

## 踩坑四：`EXPECT_OK` 复用与 `Result::Value()` 陷阱

与 TASK-003 同套 `EXPECT_OK()` 宏（见本文件第三部分），所有 `Result` 取值走 `EXPECT_OK`，
禁止裸 `.Value()`（对错误结果抛 `std::bad_variant_access`，掩盖真实断言行）。

## 手工复核清单（脚本无法自动判定，提交前逐条勾选）

- [x] 全仓 grep：sched 子树无任何 `std::thread`（连注释都不允许）；engine 无 `std::cout`/`printf`；`engine/core/src|include` 无 `throw`
- [x] 全局 `operator new` 替换：TaskFn 热路径 `heap_allocs==0`（sizeof==64、零堆分配）
- [x] MpmcQueue 4p×4c×10 万无丢失、无重复（received==400000, duplicates==0）
- [x] 调度器周期 catch-up 限幅 8 轮、无死亡螺旋（`Tick(+120ms)` 触发数 == 8）
- [x] 调度器全取消后槽位回收（SlotCount 5000→5000，无泄漏）
- [x] ObjectPool 单线程归属；Arena `Reset()` 回收；MemoryPool magic 守卫拦截 double-free
- [x] benchmark 四项达标：`sched_tick_us_10k_timers=1.387`(≤200) / `pool_acquire_release_ns=0.429`(≤20) / `arena_push_ns=2.647`(<3) / `mempool_alloc_ns=6.032`(<15)
- [x] Debug / Release 双构建通过，`ctest -R Core_Thread` 全绿

---

# 五、Core Command / Query / Event Bus（TASK-007 · `tests/bus_test.cpp` + `demo_pipeline.cpp`）

两个可执行文件覆盖三类验证，ctest 用例名 `Core_Bus.Suite`（14 个用例）与 `Core_Bus.Demo`（演示链路）。
`Core_Bus.Suite` 覆盖 §16 单测 / §17 集成 / §19 Failure；`Core_Bus.Demo` 是 §15.8 的
完整链路演示（Command → Handler → Event → 2 订阅者）。共享夹具在 `tests/bus_fixtures.h`：
`MovePlayerCommand`（5 审计字段齐全）、`GetPositionQuery`、`PlayerMovedEvent`（32B 内联）、
`EconomyEvent`（`kCritical=true`）、`BigEvent`（>32B 堆路径）。

## 测试目标与入口

| 目标 | 命令 | 说明 |
|---|---|---|
| 全量单测 + 集成 + 失败测试 | `ctest -R Core_Bus` | Suite + Demo 共 2 项 |
| 直接跑（CWD = 仓库根） | `./build/Release/bin/core_bus_test.exe` | 纯断言，无落盘 |
| 演示链路 | `./build/Release/bin/core_bus_demo.exe` | 打印 Command/Event/Query 链路结果 |
| 压测 | `./build/Release/bin/bus_bench.exe --iterations 1000000` | 写 `bench/core_bus.txt` |

## §16 单元测试

| 用例 | 断言要点 |
|---|---|
| `TestCommandRegisterDispatch` | 注册 `MovePlayerCommand` 后 `Dispatch` 返回 `Ok` 且 handler 收到正确 payload 与 context；`RegisteredCount()==1`；结果值正确 |
| `TestCommandDuplicateRegisterNotOverwrite` | 同一 Command 类型二次注册返回错误（`INVALID_ARGUMENT`），`RegisteredCount()` 不变，**第一次的 handler 仍生效**（无静默覆盖） |
| `TestCommandNotFound` | 未注册的命令 `Dispatch` → `NOT_FOUND` |
| `TestCommandHandlerThrows` | handler 抛异常 → `Dispatch` 返回 `INTERNAL_ERROR`（不崩溃、不逃逸）；再派发一个正常命令仍成功（总线状态未坏） |
| `TestQueryAskAndErrorPropagation` | 注册 `GetPositionQuery` 后 `Ask` 返回正确位置；未注册查询 → `NOT_FOUND`；query handler 抛异常 → `INTERNAL_ERROR` |
| `TestQueryNoSideEffect` | handler 内对 `SideEffectProbe::Write()`（Ask 区间内）→ `Violations()==1`（违规被抓）；Ask 之外 `Write()` 只累加 `Writes()`、不违规 |
| `TestEventMulticastOrder` | 2 个订阅者按**注册顺序**收到同一事件（first 先于 second）；payload 一致 |
| `TestEventUnsubscribeIdempotent` | `Unsubscribe(id)` 后该订阅者不再收到；重复退订同一 id / 非法 id 返回 `Ok`（幂等不崩） |
| `TestBigEventHeapPath` | `BigEvent`（>32B）走堆路径：订阅、发布、Drain 后 payload 完整一致（inline / heap 两路径行为一致） |
| `TestCrossThreadTracePropagation` | 4 个 worker 线程各发布事件（带各自 trace_id），Drain 后每个订阅回调断言 trace_id 与发布线程一致（跨线程透传无错乱） |

## §17 集成测试

| 用例 | 断言要点 |
|---|---|
| `TestEventDrainBudgetAndRemaining` | 压入 N 个事件，`Drain(max_events=小, budget=小)` 在预算/条数内停，返回值 = 剩余队列深度；继续 Drain 能清空；`QueueDepth()` 归零 |
| `TestEventBackpressure` | 队列容量故意压小：非关键事件塞满 → 返回 `Ok` 且 `DroppedCount` 递增（丢弃+计数）；`EconomyEvent`（关键）塞满 → 返回 `BUSY` 且 `DroppedCount` **不变**（关键事件绝不丢） |
| `TestMixedLoadNoDeadlock` | 4 线程 × 各 2 万事件混合发布 + 2 订阅者 + 主线程持续 Drain + 同时注册/退订新订阅者，跑完无死锁、无崩溃、总量一致 |

## §19 Failure 测试

| 用例 | 断言要点 |
|---|---|
| `TestEventSubscriberExceptionIsolation` | 第一个订阅者抛异常 → `SubscriberErrors` 计数 +1，**第二个订阅者照常收到**（坏订阅者不拖垮总线） |

## §15.8 演示链路（`Core_Bus.Demo`）

1. 注册 `MovePlayerCommand` handler：更新 `SceneState` 并 `Publish(PlayerMovedEvent)`；
2. `Dispatch` 命令 → 断言返回 `Ok`；
3. `Drain` 分发事件 → 2 个订阅者（AOI 通知 + 统计）**按注册顺序**收到，payload 与
   `request_id` 串联一致（Command → Event 同一 request）；
4. `Ask(GetPositionQuery)` → 位置正确且 `SideEffectProbe.Violations()==0`（Query 无副作用）；
5. 100 个 `EconomyEvent` 关键事件 → 全部派发、`DroppedCount()==0`（关键事件零丢失）。

## 踩坑记录（真实排障，写测试时才发现的）

1. **进程级静态 `EventTypeInfo` 缓存实例级槽位下标 → `Core_Bus.Suite` SIGSEGV**

   初版 `EventTypeInfo` 里带 `std::atomic<uint32_t> slot`（订阅槽下标缓存）。
   第一个 EventBus 实例全绿；第二个实例订阅同一事件类型时读到旧下标，`slots_[slot]`
   越界段错误（gdb：`EventBus::Subscribe<PlayerMovedEvent>` ← `TestEventUnsubscribeIdempotent`）。
   修复：下标移入实例级 `slot_index_` 表。**教训：进程级静态对象不得缓存实例级状态。**

2. **GCC 嵌套 struct 的 NSDMI 不能作默认实参**

   `EventBus` 嵌套 `Options` 带默认成员初始化时，GCC 报
   「default member initializer for 'queue_capacity' required before the end of its
   enclosing class」。用最小复现（`opt_probe2/3.cpp`）定位后，把 `Options` 提到
   命名空间级。**教训：嵌套类 NSDMI 属 complete-class context，外层类未结束前不可用。**

3. **throw-only lambda 在 `-O3` 下可能掉出函数尾（潜在 UB）**

   只含 `throw` 的 lambda 若返回 `Result<T>`，`-O3` 不保证补隐式 return。
   每个 throw 后补不可达的 `return Result<T>::Ok(T{})` 消除告警面。

4. **零丢弃断言要求测试容量足够大**

   `TestMixedLoadNoDeadlock` 初版 `queue_capacity=1<<16`（65536），4×20000=80000 事件
   必然溢出，「zero dropped」断言是**竞态运气**。改成 `1u << 17` 后语义才成立。
   写零丢弃断言前先算容量：事件总量必须 < 队列容量。

## 手工复核清单（脚本无法自动判定，提交前逐条勾选）

- [ ] Demo 链路跑通：Command → Handler → Event → 2 个 Subscriber（`Core_Bus.Demo`）
- [ ] Query 无副作用：`SideEffectProbe` 可捕获只读区间内的写入（`TestQueryNoSideEffect` 断言 `Violations()==1`）
- [ ] 重复注册同一 Command 返回错误，未静默覆盖（`TestCommandDuplicateRegisterNotOverwrite` 断言仍执行第一个 handler）
- [ ] 队列满时非关键事件丢弃计数、关键事件返回 `BUSY`（`TestEventBackpressure` 断言 `DroppedCount` 不变）
- [ ] 一个订阅者抛异常不影响其他订阅者（`SubscriberErrors` 计数 + 后续照常收到）
- [ ] EventBus 不创建线程，Drain 由宿主驱动且带时间预算（验收脚本已 grep 验证 `src/bus` 无 `std::thread`）
- [ ] benchmark 四项指标写入 `docs/PERFORMANCE.md`（24.854 / 7.620 / 11.034 / 0.000 / 48.00MB）
- [ ] Debug / Release 双构建通过，`ctest -R Core_Bus` 全绿
