# TASK-013 · SimulationScheduler — Performance Notes

> 验收阈值（TASK-013 §20 / §24）：`tick_overhead_ns ≤ 20000`、`drift_us_per_10min ≤ 50000`。
> 实测数字见末尾「本地实测」，由 `bin/sched_sim_bench --ticks 12000` 产出 `bench/sched_sim.txt`。

## 1. 开销构成（§10 / §22）

| 项 | 说明 | 预算 |
|----|------|------|
| 阶段计时 Record | 每阶段一次 `MonotonicClock::Now()`（QPC ≈ 17ns）+ 直方图 O(1) 桶写入 | < 50ns / 阶段（§22） |
| 分位快照 | 固定桶直方图 4109 桶扫描，**不进每 Tick 热路径**，仅 `Timings()` 调用时算（惰性） | 0 / Tick |
| 帧 Arena Reset | bump 指针归零，O(1) | 忽略 |
| 事件 Drain | 队列空立即 `TryPop` break，不阻塞预算 | 忽略（空队列） |
| 整数 TickClock | 纯整数步进，零浮点、零墙钟反馈 | 忽略 |

## 2. 漂移模型（§15.4）

- 调度 deadline 序列由纯整数 `TickClock` 生成：`next += interval_ns`，**无任何墙钟反馈**，故结构性零累积漂移。
- `RunUntil` 手动驱动：每次推进一个 interval 恰好得 1 个 CatchUp step（floor 除法，`delta < interval → 0`），便于确定性测试。
- `drift_us_per_10min` 测量的是**驱动方** `d += interval` 与理想固定步长的偏差（由 `MonotonicClock` 量化误差引入，与调度器内部无关），应恒为 0 量级。

## 3. CatchUp 限幅（§15.4 / §21）

- 单帧最多补 `max_catchup=3` 个 Tick。注入 5s 空档（=100 步）：实际只补 3 Tick，杜绝卡顿后死亡螺旋。
- 限幅发生在 `RunUntil` 内部：`steps = min(max_catchup, clock_.CatchUpSteps(deadline, last_fired_))`。

## 4. 长稳（§20 验收 #3）

- 12000 Tick @ 20Hz = 10 分钟。纯整数 TickClock 保证 `TickNumber() == 12000 ± 0`，
  空阶段下 `OverrunCount() == 0`，`CpuUtilization() ∈ [0,1]`。

## 5. 本地实测（MSYS2 MinGW g++ 16.1.0，Ninja，Release，`--ticks 12000`）

| 指标 | 阈值 | 实测 |
|------|------|------|
| `tick_overhead_ns`（空 stage 开销，含阶段计时） | ≤ 20000 | **2145.575** |
| `timing_overhead_ns_per_phase`（阶段计时自身开销） | — | **19.537** |
| `drift_us_per_10min`（10 分钟漂移） | ≤ 50000 | **0.000000** |

解读：
- 空 stage 每 Tick 开销 ~2.15μs（含 8 阶段计时 Record + 帧 Arena Reset + 事件 Drain），远低于 20μs 验收线；阶段计时自身仅占 ~19.5ns/阶段，符合 §22 <50ns/阶段预算。
- 漂移恒为 0：纯整数 TickClock 无墙钟反馈，结构零累积漂移（验收 #3 长稳 12000 Tick 无漂移）。

> 单测 8 函数 / 40+ 断言全绿（ctest -R Sched_Sim，4.01s），Debug + Release 双构建零警告。
