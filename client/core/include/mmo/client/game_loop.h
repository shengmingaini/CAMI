#pragma once

/// TASK-034 · 固定步长游戏循环（60Hz 逻辑帧 + CatchUp 限幅 + 帧统计）。
///
/// 设计要点：
///   - 累加器固定步长：每经过 fixed_dt 的逻辑预算就执行一次 tick；剩余时间用于渲染插值。
///   - CatchUp 限幅：单帧最多补算 max_catchup 个逻辑步，防止「螺旋死亡」
///     （慢机器/卡顿后疯狂补帧导致更卡）。
///   - 渲染回调 render(alpha) 的 alpha = 当前帧内已消耗 / 单步时长，供表现层插值。
///   - 全程使用 core::MonotonicClock，绝不用墙钟驱动（PROJECT_REQUIREMENTS §13）。
///   - 客户端不依赖服务端模块。

#include "mmo/client/stats.h"
#include "mmo/client/types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace mmo { namespace client {

class GameLoop {
public:
    using TickFn   = std::function<void(core::SteadyNs now, core::SteadyNs dt)>;
    using RenderFn = std::function<void(core::SteadyNs now, double alpha)>;

    /// 运行固定步长循环，直到 duration_ms 到期或 stop 置位。
    /// tick：逻辑步回调；render：每帧渲染回调（可为空）。
    /// 返回帧统计（帧间隔 P50/P95/P99、追帧计数）。
    static FrameStats Run(const GameLoopConfig& cfg,
                          std::uint64_t duration_ms,
                          TickFn tick,
                          RenderFn render = {},
                          const std::atomic<bool>* stop = nullptr);
};

}}  // namespace mmo::client
