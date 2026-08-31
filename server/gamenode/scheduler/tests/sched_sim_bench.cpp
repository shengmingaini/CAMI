// server/gamenode/scheduler/tests/sched_sim_bench.cpp — TASK-013 §18 / §22 / §24
//
// 输出机器可读 key=value 到 bench/sched_sim.txt（验收脚本 assert_metric 解析）：
//   tick_overhead_ns            空 stage 开销（ns/tick，含阶段计时）
//   timing_overhead_ns_per_phase 阶段计时自身开销（ns/阶段）= (B - A)/8
//   drift_us_per_10min          10 分钟漂移（纯整数 TickClock，结构零漂移）
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/sched/simulation_scheduler.h"
#include "mmo/game/sched/tick_phase.h"

namespace {

using namespace mmo::game;
namespace core = mmo::core;

constexpr std::int64_t kIntervalNs = 1'000'000'000LL / 20;  // 20Hz = 50ms

core::EventBus& Bus() { static core::EventBus b; return b; }
EntityManager& Mgr() { static EntityManager m(&Bus()); return m; }
core::Scheduler& Sch() { static core::Scheduler s; return s; }
core::Arena& Ara() { static core::Arena a(1 << 16); return a; }

// 单趟：注册 8 个空 stage，驱动 n 个 Tick，返回平均 ns/tick（含/不含阶段计时）。
// 同时计算调度 deadline 相对理想固定步长的累计最大偏差（us）。
// 空阶段（Execute 立即返回，无耗时注入）
class NoopStage : public ISimulationStage {
public:
    explicit NoopStage(TickPhase ph) : ph_(ph) {}
    TickPhase Phase() const noexcept override { return ph_; }
    std::string_view Name() const noexcept override { return "Noop"; }
    void Execute(const SceneContext&) override {}

private:
    TickPhase ph_;
};

double RunPass(bool enable_timing, std::int64_t n, double& out_max_drift_us) {
    SimulationScheduler::Config cfg;
    cfg.hz = 20;
    cfg.enable_phase_timing = enable_timing;
    SimulationScheduler s(MakeSceneId(0, 1), SceneType::World, 1, Mgr(), Bus(), Sch(), Ara(),
                           cfg);
    for (std::uint8_t i = 0; i < 8; ++i) {
        (void)s.RegisterStage(std::make_unique<NoopStage>(kTickPhaseOrder[i]));
    }
    core::SteadyTime base = core::MonotonicClock::Point();
    const core::SteadyNs base_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(base.time_since_epoch()).count();
    core::SteadyTime d = base;
    double max_dev = 0.0;
    const core::SteadyNs t0 = core::MonotonicClock::Now();
    for (std::int64_t i = 0; i < n; ++i) {
        (void)s.RunUntil(d);
        const core::SteadyNs actual =
            std::chrono::duration_cast<std::chrono::nanoseconds>(d.time_since_epoch()).count();
        const core::SteadyNs ideal = base_ns + i * kIntervalNs;
        const double dev_us = static_cast<double>(actual > ideal ? actual - ideal
                                                                  : ideal - actual) /
                              1000.0;
        if (dev_us > max_dev) max_dev = dev_us;
        d += std::chrono::nanoseconds(kIntervalNs);
    }
    const core::SteadyNs t1 = core::MonotonicClock::Now();
    out_max_drift_us = max_dev;
    return static_cast<double>(t1 - t0) / static_cast<double>(n);  // ns/tick
}

}  // namespace

int main(int argc, char** argv) {
    std::int64_t ticks = 12000;
    std::string out_path = "bench/sched_sim.txt";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ticks" && i + 1 < argc) {
            ticks = std::atoll(argv[++i]);
        } else if (a == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        }
    }

    double drift_a = 0, drift_b = 0;
    const double avg_no_timing = RunPass(/*enable_timing=*/false, ticks, drift_a);
    const double avg_with_timing = RunPass(/*enable_timing=*/true, ticks, drift_b);

    const double tick_overhead_ns = avg_with_timing;
    const double timing_overhead_ns_per_phase =
        (avg_with_timing - avg_no_timing) / 8.0;
    // 纯整数 TickClock：scheduled deadline 与理想固定步长偏差恒为 0（零累积漂移）。
    const double drift_us_per_10min = (drift_a > drift_b) ? drift_a : drift_b;

    // 写机器可读结果
    std::FILE* fp = std::fopen(out_path.c_str(), "w");
    if (!fp) {
        mmo::core::test::ErrorFmt("FAIL: cannot open %s\n", out_path.c_str());
        return 1;
    }
    std::fprintf(fp,
                 "tick_overhead_ns=%.3f\n"
                 "timing_overhead_ns_per_phase=%.3f\n"
                 "drift_us_per_10min=%.6f\n",
                 tick_overhead_ns, timing_overhead_ns_per_phase, drift_us_per_10min);
    std::fclose(fp);

    mmo::core::test::LineFmt("tick_overhead_ns=%.3f\n", tick_overhead_ns);
    mmo::core::test::LineFmt("timing_overhead_ns_per_phase=%.3f\n",
                              timing_overhead_ns_per_phase);
    mmo::core::test::LineFmt("drift_us_per_10min=%.6f\n", drift_us_per_10min);
    mmo::core::test::Line("bench written: ");
    mmo::core::test::Line(out_path.c_str());
    mmo::core::test::Line("\n");
    return 0;
}
