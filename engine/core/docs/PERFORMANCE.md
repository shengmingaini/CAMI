# engine/core · 性能实测（TASK-001 / TASK-002 / TASK-003）

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
