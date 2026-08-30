# engine/core · 性能实测（TASK-001 / TASK-002）

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
