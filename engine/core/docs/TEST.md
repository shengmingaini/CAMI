# engine/core · 测试说明（TASK-002 · Core Logger / Trace）

> TASK-001（Error/Result）的测试在 `tests/error_test.cpp`，风格与本文件一致。
> 两者共用 `tests/test_print.h` 作为输出通道（红线禁止 `printf` / `cout` / `cerr`）。

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
