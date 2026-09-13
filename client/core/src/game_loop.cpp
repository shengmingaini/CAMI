/// TASK-034 · GameLoop 实现（固定步长 + 累加器 + CatchUp 限幅）。

#include "mmo/client/game_loop.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace mmo { namespace client {

FrameStats GameLoop::Run(const GameLoopConfig& cfg,
                         std::uint64_t duration_ms,
                         TickFn tick,
                         RenderFn render,
                         const std::atomic<bool>* stop) {
    FrameStats stats;
    std::vector<double> intervals;
    std::vector<double> tick_intervals;

    const core::SteadyNs fixed = cfg.fixed_dt_ns;
    const int max_catch = std::max(1, cfg.max_catchup);

    const auto t0_ns = core::MonotonicClock::Now();
    const auto end_ns = t0_ns + static_cast<core::SteadyNs>(duration_ms) * 1'000'000;
    core::SteadyNs acc = 0;
    core::SteadyNs last = t0_ns;
    core::SteadyNs sim_time = 0;  // 已模拟的逻辑时间（用于 alpha 计算）
    double prev_tick_ms = 0.0;

    while (true) {
        const core::SteadyNs now = core::MonotonicClock::Now();
        if (stop && stop->load()) break;
        if (now >= end_ns) break;

        const core::SteadyNs frame_delta = now - last;
        last = now;
        intervals.push_back(static_cast<double>(frame_delta) / 1e6);

        acc += frame_delta;
        int steps = 0;
        while (acc >= fixed) {
            const double tk = NowMs();
            if (prev_tick_ms > 0.0) tick_intervals.push_back(tk - prev_tick_ms);
            prev_tick_ms = tk;
            if (tick) tick(sim_time, fixed);
            sim_time += fixed;
            acc -= fixed;
            ++steps;
            if (steps >= max_catch) {
                ++stats.catchups;
                acc = 0;  // 丢弃积压，避免螺旋死亡
                break;
            }
        }
        if (steps > 1) ++stats.overshoot;

        const double alpha = (fixed > 0)
            ? std::min(1.0, static_cast<double>(acc) / static_cast<double>(fixed))
            : 0.0;
        if (render) render(now, alpha);
        ++stats.frames;

        // 渲染绑定（无逻辑步）：让出时间片，避免空转 100% CPU。
        // 注意：不能用固定 sleep_for(1ms) —— Windows 默认定时器粒度约 15.6ms，
        // 会把每个空闲帧拉长到 ~15.6ms；而 fixed=16.67ms 时偶发两帧空闲之间才
        // 触发一次 tick（~31ms），使 §12 P95 超出 30ms 预算。改用 yield，由真实
        // 单调时钟驱动的累加器保证 tick 仍以 ~16.67ms 稳定节拍触发。
        if (steps == 0) std::this_thread::yield();
    }

    // 末帧/末步间隔可能不完整（被 stop/end 截断），剔除避免污染百分位。
    if (!intervals.empty()) intervals.pop_back();
    if (!tick_intervals.empty()) tick_intervals.pop_back();
    const FrameStats computed = ComputeFrameStats(intervals);
    stats.frame_ms_p50 = computed.frame_ms_p50;
    stats.frame_ms_p95 = computed.frame_ms_p95;
    stats.frame_ms_p99 = computed.frame_ms_p99;
    stats.frame_ms_max = computed.frame_ms_max;
    FillTickStats(stats, tick_intervals);
    return stats;
}

}}  // namespace mmo::client
