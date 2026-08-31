# TASK-013 · SimulationScheduler — Interface Contract

> 模块：`server/gamenode/scheduler`（GameNode 子树第三个模块，前两个为 TASK-011 entity、TASK-012 scene）
> 命名空间：`mmo::game`
> 职责（§4 State Owner）：独占 TickNumber 与八阶段推进权，按固定顺序驱动各 SimulationStage。

## 1. 公开接口（冻结契约，见 TASK-013 §27.1）

### 1.1 阶段枚举与固定顺序 — `tick_phase.h`

```cpp
enum class TickPhase : uint8_t {
    Input = 0, Movement, Aoi, Combat, Buff, Quest, Event, Replication, Count
};

constexpr const char* ToString(TickPhase) noexcept;          // switch 覆盖 Count→"Unknown"
constexpr uint8_t     TickPhaseIndex(TickPhase) noexcept;     // = 底层 uint8_t 值
inline constexpr std::array<TickPhase, 8> kTickPhaseOrder;    // 写死顺序，禁止运行期调整
```

- `kTickPhaseOrder` = `{Input, Movement, Aoi, Combat, Buff, Quest, Event, Replication}`。
- 顺序由编译期常量固定；任何运行期重排都被视为破坏契约（§1 / §21）。

### 1.2 阶段接口 — `simulation_stage.h`

```cpp
class ISimulationStage {
public:
    virtual ~ISimulationStage() = default;
    virtual TickPhase      Phase() const noexcept = 0;        // 返回所属阶段
    virtual void           Execute(const SceneContext&) = 0;  // 单阶段逻辑，禁止阻塞/IO（§9/§10）
    virtual std::string_view Name() const noexcept = 0;       // 诊断用
};
```

### 1.3 配置与调度器 — `simulation_scheduler.h`

```cpp
struct SimulationScheduler::Config {
    std::uint32_t   hz;                 // 默认 20
    std::uint32_t   max_catchup;        // 默认 3（§15.4 / §21 限幅，杜绝死亡螺旋）
    core::DurationMs event_budget;      // 默认 2ms（每 Tick 事件派发预算，§4/§7）
    bool            enable_phase_timing;// 默认 true（§20 验收 #2）
};

SimulationScheduler(SceneId, SceneType, NodeId owner_node,
                    EntityManager&, core::EventBus&, core::Scheduler&,
                    core::Arena& frame_arena, Config = {});

core::Result<void> RegisterStage(std::unique_ptr<ISimulationStage>); // 按 Phase 排序插入；重复 Phase → INVALID_ARGUMENT（§21 禁止静默覆盖）
core::Result<void> Start();                  // 起后台 Simulation 线程，按 20Hz 自驱（§9）
void               Stop() noexcept;          // 完成当前 Tick 后退出，不中途杀（§19）
core::Result<void> RunUntil(core::SteadyTime deadline); // 手动驱动，推进所有 deadline<=给定时刻的 Tick（测试用，§15.9）

const std::array<PhaseTiming, 8>& Timings() const noexcept; // 惰性快照：调用时才由累积直方图算（§22 计时预算）
std::uint64_t TickNumber() const noexcept;                  // 已完成 Tick 数
std::uint64_t OverrunCount() const noexcept;                // 总耗时 >50ms 的 Tick 计数（§15.8）
double        CpuUtilization() const noexcept;              // busy_ns/wall_ns ∈ [0,1]
std::uint64_t MaxTickUs() const noexcept;                   // 历史最大单 Tick 耗时（μs）
```

### 1.4 阶段计时快照 — `tick_timing.h`

```cpp
struct PhaseTiming {
    TickPhase   phase;
    std::uint64_t last_us;  // 最近一次该阶段耗时（μs）
    std::uint64_t avg_us;   // 平均耗时（μs）
    std::uint64_t p95_us;   // 95 分位（μs）
    std::uint64_t p99_us;   // 99 分位（μs）
    std::uint64_t max_us;   // 最大耗时（μs）
};
```

- 分位数用**固定桶直方图**（0..4095μs 按 1μs 分 4096 桶，≥4096μs 按 2 幂分 13 粗桶，共 4109 桶），**禁止每次排序**（§15.3 / §21）。
- `Record(us)` 为 O(1) 桶映射；`Snapshot()` 为 O(桶数) 累积，仅在 `Timings()` 调用时算（不进每 Tick 热路径，§22）。

## 2. 调度模型（§9 / §15.4）

- 基于 TASK-003 `TickClock` 的**纯整数固定步长**：`next += interval_ns`，零累积漂移。
- `CatchUpSteps` 用 floor 除法，`delta < interval → 0`；单帧最多补 `max_catchup`（默认 3）个 Tick，防卡顿后死亡螺旋。
- 每 Tick 开始 `arena_.Reset()`（TASK-004 帧 Arena bump 分配，无逐个析构）。
- 单阶段 `Execute` 抛异常被 `try/catch(...)` 捕获跳过，后续阶段继续（§19）。
- Tick 总耗时 >50ms（`kTickOverrunNs`）计入 `OverrunCount` 并 CAS 更新 `MaxTickUs`（§15.8）。
- 事件派发：`events_.Drain(4096, event_budget)`，预算耗尽立即返回剩余，留到下一帧（§4/§7，禁止 Tick 内无限派发）。

## 3. 线程模型（§9 / §19）

- 每 Scene 一个实例，绑定固定 SimulationThread（`Start` 后台线程，`std::this_thread::sleep_until` 到下一个 deadline）。
- `Stop` 在持锁检查 `running_` 为假后 break，**完成当前正在执行的 Tick**再退出，不中途杀。
- `RunUntil` 为测试提供手动驱动（不占线程，禁止测试依赖 sleep）。

## 4. 依赖方向（§27.3）

```
server/gamenode/scheduler
  ├─ engine/core  : error / bus / memory(Arena) / sched / time(TickClock, MonotonicClock)
  ├─ server/gamenode/entity : EntityManager（§27.2 消费公开接口）
  └─ server/gamenode/scene  : SceneContext / SceneId / SceneType（§27.2）
```

- 红线：公开头（`include/`）禁止 `#include` 本模块 `src/`；禁止全局锁；禁止在 STATUS:DONE 后静默改接口签名（须走 `version` + 兼容性评估）。
