// engine/core/tests/sched_bench.cpp — TASK-004 Scheduler Benchmark
//
// 输出机器可读 key=value 到 stdout 与 bench/core_sched.txt（供验收脚本 assert_metric 解析）。
// 阈值（§22）：sched_tick_us_10k_timers <= 200（每 Tick 扫描 1 万个定时器的耗时，单位 µs）。
//
// 计时口径：用 MonotonicClock 自测（不引入 steady_clock 额外开销偏差）。
// 反优化：定时器回调里对 fired 计数；计数在读端被消费，避免 -O3 把 Tick 循环整体消除。

#include "mmo/core/sched/scheduler.h"
#include "mmo/core/thread/task.h"
#include "mmo/core/time/clock.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "test_print.h"

namespace {

namespace tprint = ::mmo::core::test;
using mmo::core::DurationMs;
using mmo::core::MonotonicClock;
using mmo::core::Scheduler;
using mmo::core::SteadyNs;
using mmo::core::SteadyTime;
using mmo::core::TaskFn;

double NsPerOp(SteadyNs total_ns, std::size_t iterations) {
    return static_cast<double>(total_ns) / static_cast<double>(iterations);
}

volatile std::int64_t g_sink = 0;

}  // namespace

int main(int argc, char** argv) {
    std::size_t timers = 10000;
    std::size_t ticks = 1000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--timers") == 0 && (i + 1) < argc) {
            timers = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
        } else if (std::strcmp(argv[i], "--ticks") == 0 && (i + 1) < argc) {
            ticks = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
        }
    }
    if (timers == 0) {
        timers = 10000;
    }
    if (ticks == 0) {
        ticks = 1000;
    }

    Scheduler sched;
    std::size_t fired = 0;   // 回调里 ++，读端消费，防止 -O3 消除 Tick 循环

    // 注册 timers 个一次性定时器，deadline 均匀撒在 [t0, t0 + 2*ticks*interval) 区间，
    // 保证整个测量窗口内堆里始终维持 ~1 万个待触发定时器（贴近 50k 并发的真实负载）。
    const DurationMs interval(1);  // 每 Tick 推进 1ms
    const SteadyTime t0 = MonotonicClock::Point();
    std::minstd_rand rng(0x9E3779B9ULL);
    const std::uint64_t span_ns =
        static_cast<std::uint64_t>(ticks) * 2ULL * 1000000ULL;  // 2*ticks ms，单位 ns

    for (std::size_t i = 0; i < timers; ++i) {
        const std::uint64_t r = static_cast<std::uint64_t>(rng()) % span_ns;
        const SteadyTime when =
            t0 + std::chrono::nanoseconds(static_cast<std::int64_t>(r));
        // 捕获 fired 的指针（8 字节，内联进 TaskFn 存储区），回调里自增。
        (void)sched.ScheduleAt(when, TaskFn([&fired] { ++fired; }));
    }

    // ---- 测量：跑 ticks 个 Tick，累计纯 Tick 耗时 ----
    SteadyTime now = t0;
    const SteadyNs start = MonotonicClock::Now();
    for (std::size_t tk = 0; tk < ticks; ++tk) {
        now += interval;                 // 每 Tick 把单调时刻推进 1ms
        (void)sched.Tick(now);
    }
    const SteadyNs elapsed = MonotonicClock::Now() - start;

    const double us_per_tick =
        NsPerOp(elapsed, ticks) / 1000.0;  // ns/op -> µs/tick
    g_sink = static_cast<std::int64_t>(fired);

    char buf[1024];
    const int written = std::snprintf(
        buf, sizeof(buf),
        "sched_tick_us_10k_timers=%.3f\n"
        "sched_fired=%zu\n"
        "sched_timers=%zu\n"
        "sched_ticks=%zu\n"
        "sched_total_us=%.3f\n",
        us_per_tick, fired, timers, ticks,
        static_cast<double>(elapsed) / 1000.0);
    if (written > 0) {
        tprint::Write(buf, static_cast<std::size_t>(written), stdout);
    }

    std::FILE* file = std::fopen("bench/core_sched.txt", "w");
    if (file != nullptr) {
        std::fputs(buf, file);
        std::fclose(file);
    } else {
        tprint::Error("WARN: cannot write bench/core_sched.txt\n");
        return 1;
    }
    return 0;
}
