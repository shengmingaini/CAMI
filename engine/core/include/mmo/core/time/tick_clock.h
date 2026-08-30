#pragma once

#include <cstdint>

#include "mmo/core/time/clock.h"

namespace mmo::core {

/// TickClock —— 固定频率 Tick 的**纯计算**调度器。
///
/// State Owner  : 无状态（除构造参数外），可多线程共享只读；按 PROJECT_REQUIREMENTS §13
///                只在 SimulationThread 使用，因此不加锁。
/// Hot Path     : YES —— 每 Tick 调用一次 NextTickDeadline / CatchUpSteps。
/// Thread Safety: 所有方法 const、只读成员，任意线程并发调用安全。
///
/// 关键设计：
///   1. 本类**不主动读时钟**，只基于调用方传入的 SteadyTime 做纯整数运算，
///      因此外部注入任何时钟（含回拨的墙钟）都无法影响其 deadline 序列。
///   2. 间隔用整数纳秒累加（deadline += interval_ns），不是「now + interval」，
///      因此不存在累积漂移：跑 10000 Tick 的累计误差理论为 0。
///   3. CatchUpSteps 限幅，单帧最多补 kMaxCatchUpSteps 个 Tick，防止卡顿后
///      一次性补太多 Tick 造成「死亡螺旋」。
class TickClock final {
public:
    /// 单帧最多补跑的 Tick 数（防死亡螺旋）。
    static constexpr std::uint32_t kMaxCatchUpSteps = 3;

    /// 默认 Tick 频率：PROJECT_REQUIREMENTS §13 规定的 20Hz。
    static constexpr std::uint32_t kDefaultHz = 20;

    /// hz 为 0 时退化为 1Hz（防御，禁止除零）；hz > 1e9 时间隔下限截为 1ns。
    explicit TickClock(std::uint32_t hz) noexcept;

    std::uint32_t Hz() const noexcept { return hz_; }

    /// Tick 间隔（整数纳秒）。非整除时向下取整，截断误差 < 1ns/Tick。
    SteadyNs TickIntervalNs() const noexcept { return interval_ns_; }

    /// Tick 间隔（毫秒）。仅用于展示与日志，调度请用 TickIntervalNs()。
    DurationMs TickInterval() const noexcept;

    /// 下一个 Tick 的绝对 deadline。严格 = prev + TickIntervalNs()，与墙钟无关。
    SteadyTime NextTickDeadline(SteadyTime prev) const noexcept;

    /// 从 prev 到 now 之间应补跑的 Tick 数，上限 kMaxCatchUpSteps。
    /// now <= prev 时返回 0（单调时钟下不应发生，作防御）。
    std::uint32_t CatchUpSteps(SteadyTime now, SteadyTime prev) const noexcept;

private:
    std::uint32_t hz_;
    SteadyNs interval_ns_;
};

}  // namespace mmo::core
