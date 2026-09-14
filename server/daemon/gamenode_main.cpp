// server/daemon/gamenode_main.cpp — GameNode 进程入口（§3 进程组合层）
//
// 组合：TASK-004 Scheduler（定时器）+ core EventBus（跨模块事件）+ 14 个 gamenode
// 模块静态库（entity/scene/sched/aoi/movement/role/inventory/ai/quest/world/combat/
// economy/social/scene_recovery）。第一版 daemon 只建立「进程骨架 + 20Hz 主循环 +
// 事件派发 + 定时器」，各模块的业务接线（Scene 装载 / Entity 生成）由对应 gameplay
// 任务在 tick 钩子里扩展 —— 本入口保证：进程可启动、可优雅退出、Tick 节拍正确。
//
// Tick 节拍（§8 固定 20Hz，tick.json 可调；单调时钟驱动，§13）：
//   Event（EventBus.Drain 2ms 预算）→ Scheduler 定时器 → 模块 Tick（空钩子占位）。
//   掉拍不补帧：对齐下一拍直接执行（§8 顺序执行优先，Scheduler 自带 catch-up 限幅）。
//
// 红线：本进程不直连 MySQL/Redis（§33）；热路径无阻塞 IO（§9）；
//       单线程固定顺序执行（§8 单 Owner + 顺序执行）；不自建线程（§21）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"

#include "daemon_common.h"

namespace {

using mmo::core::DurationMs;
using mmo::core::MonotonicClock;

}  // namespace

int main(int argc, char** argv) {
    using namespace mmo::daemon;
    namespace core = mmo::core;  // daemon_common.h 内引用 core:: 类型，main 里同样需要该别名

    Args args;
    if (!ParseArgs(argc, argv, &args) || args.help) {
        PrintUsage(argc > 0 ? argv[0] : "gamenode");
        return args.help ? 0 : 1;
    }

    InitLoggerOrWarn("gamenode");
    InstallSignalHandlers();

    LoadConfigOrWarn(args.config_dir);
    ApplyLogLevelFromConfig();

    const auto hz = static_cast<std::uint32_t>(CfgOr<std::uint32_t>("tick.hz", 20));
    const DurationMs tick_period(hz > 0 ? 1000u / hz : 50u);

    MMO_LOG_INFO("gamenode: starting (tick={}Hz period={}ms config v{})", hz,
                 tick_period.count(), core::ConfigManager::Version());

    mmo::core::EventBus bus;
    mmo::core::Scheduler scheduler;

    // 周期任务：每 10s 打一次调度器健康指标（TASK-039 采集口）。
    (void)scheduler.ScheduleEvery(DurationMs(10000), [&scheduler] {
        MMO_LOG_INFO("gamenode: sched timers={} failed_fires={} ready={}",
                     scheduler.TimerCount(), scheduler.FailedFires(), scheduler.ReadyCount());
    });

    const core::SteadyTime deadline = RunDeadline(args.run_for_sec);
    core::SteadyTime next_tick = MonotonicClock::Point();
    core::SteadyTime last_summary = MonotonicClock::Point();
    std::uint64_t ticks = 0;
    std::uint32_t tick_overruns = 0;
    std::uint64_t tick_max_us = 0;

    while (!g_stop.load(std::memory_order_relaxed) && !DeadlineReached(deadline)) {
        const auto now = MonotonicClock::Point();

        if (now >= next_tick) {
            // ---- 到拍：执行一个 Tick（20Hz §8 顺序：Event → Scheduler → 模块）----
            const auto t0 = MonotonicClock::Point();
            ++ticks;
            if (now > next_tick + tick_period) {
                ++tick_overruns;
            }
            next_tick += tick_period;

            (void)bus.Drain(1024, DurationMs(2));
            (void)scheduler.Tick(MonotonicClock::Point());
            // 模块 Tick 钩子：gameplay 任务在此扩展（Scene/AI/Combat...）

            const auto us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    MonotonicClock::Point() - t0)
                    .count());
            if (us > tick_max_us) {
                tick_max_us = us;
            }
        } else {
            // ---- 空闲：睡到下一拍（Tick 外冷路径；顺带做 5s 摘要）----
            const auto sleep_us = std::chrono::duration_cast<std::chrono::microseconds>(
                next_tick - now);
            if (sleep_us.count() > 0) {
                std::this_thread::sleep_for(
                    std::chrono::duration_cast<std::chrono::milliseconds>(sleep_us));
            } else {
                std::this_thread::yield();
            }
        }

        const auto t = MonotonicClock::Point();
        if (t - last_summary >= DurationMs(5000)) {
            MMO_LOG_INFO("gamenode: ticks={} overruns={} tick_max_us={}", ticks, tick_overruns,
                         tick_max_us);
            last_summary = t;
        }
    }

    MMO_LOG_INFO("gamenode: shutting down (ticks={})", ticks);
    core::Logger::Flush();
    core::Logger::Shutdown();
    return 0;
}
