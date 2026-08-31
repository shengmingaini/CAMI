// server/gamenode/scheduler/src/simulation_scheduler.cpp — TASK-013 §15
//
// 固定 20Hz Tick 调度：基于 TASK-003 TickClock 的纯整数固定步长 + CatchUp 限幅，
// 每 Tick 帧 Arena Reset，单阶段异常捕获不崩，Tick 总耗时 >50ms 计 Overrun，
// RunUntil 手动驱动（测试不依赖 sleep）。

#include "mmo/game/sched/simulation_scheduler.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace mmo::game {

SimulationScheduler::SimulationScheduler(SceneId scene_id, SceneType scene_type,
                                         NodeId owner_node, EntityManager& entities,
                                         core::EventBus& events, core::Scheduler& scheduler,
                                         core::Arena& frame_arena, Config config)
    : scene_id_(scene_id),
      scene_type_(scene_type),
      owner_node_(owner_node),
      entities_(entities),
      events_(events),
      scheduler_(scheduler),
      arena_(frame_arena),
      config_(config),
      clock_(config.hz) {}

core::Result<void> SimulationScheduler::RegisterStage(
    std::unique_ptr<ISimulationStage> stage) {
    if (!stage) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                     "null stage", core::domain::kCore));
    }
    const TickPhase ph = stage->Phase();
    for (const auto& s : stages_) {
        if (s->Phase() == ph) {
            // §21 禁止静默覆盖已注册 Stage：重复注册同 Phase 直接报错。
            return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                         "duplicate phase", core::domain::kCore));
        }
    }
    stages_.push_back(std::move(stage));
    // §1 固定顺序：按 Phase 排序插入，禁止运行期调整。
    std::sort(stages_.begin(), stages_.end(),
              [](const std::unique_ptr<ISimulationStage>& a,
                 const std::unique_ptr<ISimulationStage>& b) {
                  return a->Phase() < b->Phase();
              });
    return core::Result<void>::Ok();
}

void SimulationScheduler::FireTick(core::SteadyTime deadline) {
    const std::uint64_t tn = tick_number_.fetch_add(1, std::memory_order_relaxed) + 1;

    // §6 / §15.6 帧 Arena：每 Tick 开始 Reset，帧内临时对象统一回收（无逐个析构）。
    arena_.Reset();

    // §7：构造本 Tick 的运行期上下文（聚合引用，Tick 结束即失效，禁止跨 Tick 保存）。
    SceneContext ctx(scene_id_, scene_type_, owner_node_, deadline, tn, entities_, events_,
                     scheduler_, arena_);

    const core::SteadyNs t0 = core::MonotonicClock::Now();
    core::SteadyNs phase_prev = t0;

    for (auto& st : stages_) {
        try {
            st->Execute(ctx);  // §9 / §10：禁止阻塞 / 禁止 IO
        } catch (...) {
            // §19：单阶段异常不崩溃，跳过该阶段继续后续阶段，禁止整个 Tick 崩。
        }
        if (config_.enable_phase_timing) {
            const core::SteadyNs t = core::MonotonicClock::Now();
            // t / phase_prev 为纳秒（SteadyNs）；PhaseTiming 直方图与 avg_us/p95/p99/max
            // 语义均为微秒（§15.3/§21 固定桶直方图 0..4095us 按 1us 分桶），故 ns→us。
            const std::uint64_t us = static_cast<std::uint64_t>(t - phase_prev) / 1000ULL;
            timing_.Record(st->Phase(), us);  // §20 验收 #2：每阶段独立统计
            phase_prev = t;
        }
    }

    const core::SteadyNs t1 = core::MonotonicClock::Now();
    const std::uint64_t total_ns = static_cast<std::uint64_t>(t1 - t0);
    busy_ns_.fetch_add(total_ns, std::memory_order_relaxed);

    // §15.8 Overrun 统计：Tick 总耗时 >50ms 计数并记录最大耗时。
    const std::uint64_t total_us = total_ns / 1000ULL;
    if (total_us > (kTickOverrunNs / 1000ULL)) {
        overrun_count_.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t cur = max_tick_us_.load(std::memory_order_relaxed);
        while (total_us > cur && !max_tick_us_.compare_exchange_weak(cur, total_us)) {
            // 自旋 CAS 直到写入更大值或已被他人写入更大值。
        }
    }

    // §4 / §7 事件派发：带预算，超时立即返回剩余，留到下一帧（禁止无限派发）。
    (void)events_.Drain(4096, config_.event_budget);
    // 注：分位快照（timing_.Snapshot）不在此做，由 Timings() 惰性计算，避免每 Tick
    //     扫描固定桶直方图破坏 §22 阶段计时开销预算（< 50ns/阶段）。
}

core::Result<void> SimulationScheduler::RunUntil(core::SteadyTime deadline) {
    if (!seeded_) {
        // 首个 RunUntil：锚定首个 Tick 的 deadline，随后按 TickClock 推进。
        seeded_ = true;
        if (start_ns_ == 0) {
            start_ns_ = static_cast<std::uint64_t>(core::MonotonicClock::Now());
        }
        last_fired_ = deadline;
        FireTick(deadline);
        next_deadline_ = clock_.NextTickDeadline(deadline);
        return core::Result<void>::Ok();
    }
    // §15.4 / §21 CatchUp 限幅：单帧最多补 max_catchup 个 Tick，杜绝死亡螺旋。
    const std::uint32_t steps =
        std::min(config_.max_catchup, clock_.CatchUpSteps(deadline, last_fired_));
    for (std::uint32_t i = 0; i < steps; ++i) {
        const core::SteadyTime dl = clock_.NextTickDeadline(last_fired_);
        FireTick(dl);
        last_fired_ = dl;
    }
    next_deadline_ = clock_.NextTickDeadline(last_fired_);
    return core::Result<void>::Ok();
}

core::Result<void> SimulationScheduler::Start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                     "already running", core::domain::kCore));
    }
    running_ = true;
    seeded_ = true;
    start_ns_ = static_cast<std::uint64_t>(core::MonotonicClock::Now());
    last_fired_ = core::MonotonicClock::Point();
    next_deadline_ = clock_.NextTickDeadline(last_fired_);
    thread_ = std::thread([this]() { Loop(); });
    return core::Result<void>::Ok();
}

void SimulationScheduler::Stop() noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        running_ = false;
    }
    if (thread_.joinable()) thread_.join();
}

void SimulationScheduler::Loop() noexcept {
    while (true) {
        const core::SteadyTime now = core::MonotonicClock::Point();
        if (next_deadline_ > now) {
            // 未到下一个 deadline：睡眠到该时刻（标准库 sleep_until 用单调时钟）。
            std::this_thread::sleep_until(next_deadline_);
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!running_) break;  // §19：完成当前 Tick 后停止，不中途杀
        }
        (void)RunUntil(next_deadline_);
    }
}

double SimulationScheduler::CpuUtilization() const noexcept {
    if (start_ns_ == 0) return 0.0;
    const std::uint64_t now = static_cast<std::uint64_t>(core::MonotonicClock::Now());
    const std::uint64_t wall = now - start_ns_;
    if (wall == 0) return 0.0;
    double u = static_cast<double>(busy_ns_.load(std::memory_order_relaxed)) /
               static_cast<double>(wall);
    if (u > 1.0) u = 1.0;
    if (u < 0.0) u = 0.0;
    return u;
}

}  // namespace mmo::game
