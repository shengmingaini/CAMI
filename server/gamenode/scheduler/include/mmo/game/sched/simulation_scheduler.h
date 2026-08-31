#pragma once

/// TASK-013 · SimulationScheduler —— GameNode 固定 20Hz Tick 调度器（§1 / §7）。
///
/// 职责（§4 State Owner）：独占 TickNumber 与八阶段推进权。每 Tick 按固定顺序
/// Input→Movement→AOI→Combat→Buff→Quest→Event→Replication 驱动各阶段，且每阶段
/// 独立计时（§20 验收 #2）。
///
/// 调度模型（§9 / §15.4）：基于 TASK-003 TickClock 的**纯整数**固定步长，无累积漂移；
/// CatchUpSteps 限幅 max_catchup（默认 3）杜绝卡顿后的死亡螺旋（§21）。RunUntil 为
/// 测试提供手动驱动（不占线程，禁止测试依赖 sleep）。
///
/// 线程模型（§9）：每 Scene 一个实例，绑定固定 SimulationThread（Start 后台线程）。
/// Stage 执行期间不得阻塞 / 不得 IO（§10）。

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/core/time/tick_clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/sched/simulation_stage.h"
#include "mmo/game/sched/tick_timing.h"

namespace mmo::game {

/// Tick 总耗时超此值（ns）计入 Overrun（§8 / §15.8：>50ms）。
inline constexpr std::uint64_t kTickOverrunNs = 50'000'000ULL;

class SimulationScheduler {
public:
    struct Config {
        std::uint32_t hz;
        std::uint32_t max_catchup;
        core::DurationMs event_budget;
        bool enable_phase_timing;
        Config()
            : hz(20),
              max_catchup(3),
              event_budget(2),
              enable_phase_timing(true) {}
    };

    /// 构造：持有共享系统引用 + Scene 身份，用于每 Tick 构造 SceneContext。
    /// deps：EntityManager（TASK-011）/ EventBus（TASK-007）/ Scheduler（TASK-004）/
    ///       Arena（TASK-004，帧内 bump 分配）/ SceneContext 类型（TASK-012）。
    SimulationScheduler(SceneId scene_id, SceneType scene_type, NodeId owner_node,
                        EntityManager& entities, core::EventBus& events,
                        core::Scheduler& scheduler, core::Arena& frame_arena,
                        Config config = {});

    // ---- 阶段注册（§7 / §15.5） ----
    /// 注册一个阶段实例，按 Phase 排序插入；重复注册同 Phase 返回错误（禁止静默覆盖，§21）。
    core::Result<void> RegisterStage(std::unique_ptr<ISimulationStage> stage);

    // ---- 生命周期 ----
    /// 启动后台 Simulation 线程，按 20Hz 固定步长自驱（§9）。已启动返回错误。
    core::Result<void> Start();
    /// 停止后台线程：完成当前正在执行的 Tick 后退出，不中途杀（§19）。
    void Stop() noexcept;

    // ---- 手动驱动（测试用，不占线程，§7 / §15.9） ----
    /// 推进到 deadline：运行所有 deadline <= 给定时刻的待定 Tick，数量受 max_catchup 限幅。
    core::Result<void> RunUntil(core::SteadyTime deadline);

    // ---- 观测 ----
    /// 8 阶段独立统计快照（§20 验收 #2）。惰性计算：仅在调用时由累积直方图快照，
    /// 不放在每 Tick 热路径（固定桶直方图扫描 ~4k 桶，每 Tick 做会破坏 §22 计时开销预算）。
    const std::array<PhaseTiming, 8>& Timings() const noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        timings_snapshot_ = timing_.Snapshot();
        return timings_snapshot_;
    }
    std::uint64_t TickNumber() const noexcept { return tick_number_.load(std::memory_order_relaxed); }
    /// 总耗时 >50ms 的 Tick 计数（§15.8）。
    std::uint64_t OverrunCount() const noexcept { return overrun_count_.load(std::memory_order_relaxed); }
    /// 模拟线程 CPU 占用率（busy_ns / wall_ns，∈ [0,1]）。
    double CpuUtilization() const noexcept;

    // ---- 诊断只读 ----
    std::uint64_t MaxTickUs() const noexcept { return max_tick_us_.load(std::memory_order_relaxed); }

private:
    void FireTick(core::SteadyTime deadline);
    void Loop() noexcept;

    SceneId scene_id_;
    SceneType scene_type_;
    NodeId owner_node_;
    EntityManager& entities_;
    core::EventBus& events_;
    core::Scheduler& scheduler_;
    core::Arena& arena_;
    Config config_;
    core::TickClock clock_;

    std::vector<std::unique_ptr<ISimulationStage>> stages_;  // 按 Phase 排序

    std::atomic<std::uint64_t> tick_number_{0};
    std::atomic<std::uint64_t> overrun_count_{0};
    std::atomic<std::uint64_t> max_tick_us_{0};
    std::atomic<std::uint64_t> busy_ns_{0};

    mutable std::mutex mu_;
    bool seeded_{false};                 // 是否已锚定首个 Tick 的 deadline
    bool running_{false};                // Start/Stop 状态
    core::SteadyTime last_fired_{};      // 最近一次已触发 Tick 的 deadline
    core::SteadyTime next_deadline_{};   // 下一个待触发 Tick 的 deadline
    std::uint64_t start_ns_{0};          // CPU 利用率基准（MonotonicClock::Now）
    mutable TickTiming timing_;          // 8 阶段直方图（累积）
    mutable std::array<PhaseTiming, 8> timings_snapshot_{};  // Timings() 返回的稳定数组

    std::thread thread_;
};

}  // namespace mmo::game
