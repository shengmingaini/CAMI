#pragma once

/// TASK-034 · 客户端指标统计辅助（帧间隔 / RTT 百分位）。
///
/// 纯计算，无 I/O、无线程，便于单测。百分位用 std::nth_element（O(n) 平均）。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mmo { namespace client {

/// 固定步长循环的帧统计（§12 验收：帧精度 P95 / 缓冲/追帧计数）。
struct FrameStats {
    std::uint64_t frames   = 0;     // 渲染循环迭代次数
    std::uint64_t ticks    = 0;     // 逻辑步执行次数（≈ 固定 60Hz）
    std::uint64_t overshoot = 0;   // 单帧执行 > 1 个逻辑步的帧数
    std::uint64_t catchups  = 0;   // 触发追帧（达 max_catchup 上限）的次数
    double frame_ms_p50 = 0.0;
    double frame_ms_p95 = 0.0;
    double frame_ms_p99 = 0.0;
    double frame_ms_max = 0.0;
    // 逻辑步实际节拍（两次 tick 之间的真实毫秒差），即「帧精度」验收口径。
    double tick_ms_p50 = 0.0;
    double tick_ms_p95 = 0.0;
    double tick_ms_p99 = 0.0;
    double tick_ms_max = 0.0;
};

/// 百分位：p ∈ [0,100]。要求 samples 非空；空则返回 0。
inline double Percentile(std::vector<double>& samples, double p) {
    if (samples.empty()) return 0.0;
    const double clamped = std::max(0.0, std::min(100.0, p));
    std::size_t rank = static_cast<std::size_t>(
        std::round((clamped / 100.0) * static_cast<double>(samples.size() - 1)));
    if (rank >= samples.size()) rank = samples.size() - 1;
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(rank),
                     samples.end());
    return samples[rank];
}

/// 由帧间隔样本列表计算 FrameStats。
inline FrameStats ComputeFrameStats(std::vector<double>& intervals_ms) {
    FrameStats s;
    s.frames = intervals_ms.size();
    if (intervals_ms.empty()) return s;
    double mx = 0.0;
    for (double v : intervals_ms) mx = std::max(mx, v);
    s.frame_ms_max = mx;
    // 拷贝后排序求 P50/P95/P99（不破坏原样本顺序语义，这里直接排序副本）
    std::vector<double> sorted = intervals_ms;
    std::sort(sorted.begin(), sorted.end());
    s.frame_ms_p50 = Percentile(sorted, 50.0);
    s.frame_ms_p95 = Percentile(sorted, 95.0);
    s.frame_ms_p99 = Percentile(sorted, 99.0);
    return s;
}

/// 由逻辑步间隔（相邻 tick 真实毫秒差）样本计算 tick 节拍统计。
inline FrameStats& FillTickStats(FrameStats& s, std::vector<double>& tick_intervals_ms) {
    s.ticks = tick_intervals_ms.size() + (tick_intervals_ms.empty() ? 0u : 1u);
    if (tick_intervals_ms.empty()) return s;
    double mx = 0.0;
    for (double v : tick_intervals_ms) mx = std::max(mx, v);
    s.tick_ms_max = mx;
    std::vector<double> sorted = tick_intervals_ms;
    std::sort(sorted.begin(), sorted.end());
    s.tick_ms_p50 = Percentile(sorted, 50.0);
    s.tick_ms_p95 = Percentile(sorted, 95.0);
    s.tick_ms_p99 = Percentile(sorted, 99.0);
    return s;
}

}}  // namespace mmo::client
