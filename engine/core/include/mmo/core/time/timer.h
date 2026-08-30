#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"

namespace mmo::core {

using TimerId = std::uint64_t;

/// 无效的定时器句柄：Schedule 永不返回该值，调用方可用它表示「无定时器」。
inline constexpr TimerId kInvalidTimerId = 0;

/// 空队列哨兵：NextDeadlineDelayNs 在队列为空时返回该值，表示「可以一直睡」。
inline constexpr SteadyNs kNoDeadline = -1;

enum class TimerKind : std::uint8_t {
    OneShot = 0,   ///< 触发一次后自动注销
    Periodic = 1,  ///< 按 interval_ns 周期触发，直到 Cancel 或达到 max_fires
};

/// 定时器的不可变描述。所有时间量都基于单调时钟，禁止传入墙钟差值。
struct TimerSpec {
    TimerKind kind = TimerKind::OneShot;
    SteadyNs first_delay_ns = 0;  ///< 首次触发前的延迟（>= 0）
    SteadyNs interval_ns = 0;     ///< 周期触发间隔，仅 Periodic 有效
    std::uint32_t max_fires = 0;  ///< 最大触发次数，0 表示不限
};

/// 定时器回调：now 为触发时刻（单调时钟），id 为触发的定时器句柄。
/// 约定：回调内禁止做阻塞 IO、禁止抛异常；需要重活请投递到业务队列。
using TimerCallback = std::function<void(SteadyTime now, TimerId id)>;

/// ITimerQueue —— 基于单调时钟的定时器队列**接口**。
///
/// TASK-003 只定义接口，**不实现调度**；具体实现由 TASK-004（Scheduler）提供。
/// 这里提前固化契约，避免 Scheduler 自行发明一套时间语义。
///
/// Thread Safety : 实现方可自行决定；默认约定所有方法仅在 Scheduler 线程调用。
/// Hot Path      : FireReady 位于 Tick 循环内，实现方必须做到 O(log n) 且零阻塞 IO。
///
/// 实现方约束（TASK-004 必须遵守）：
///   1. 时间基准只能是 MonotonicClock，禁止读取 WallClock。
///   2. 回调内抛出的异常必须被捕获并记日志，禁止冒泡到 Tick 循环。
///   3. FireReady 单轮触发次数必须限幅，防死亡螺旋（参考 TickClock::kMaxCatchUpSteps）。
class ITimerQueue {
public:
    ITimerQueue() = default;
    ITimerQueue(const ITimerQueue&) = delete;
    ITimerQueue& operator=(const ITimerQueue&) = delete;
    ITimerQueue(ITimerQueue&&) = delete;
    ITimerQueue& operator=(ITimerQueue&&) = delete;
    virtual ~ITimerQueue() = default;

    /// 注册定时器。callback 为空返回 INVALID_ARGUMENT；返回句柄从 1 开始自增。
    virtual Result<TimerId> Schedule(TimerSpec spec, TimerCallback callback) = 0;

    /// 注销定时器。id 不存在返回 NOT_FOUND（幂等语义由实现方决定，默认 NOT_FOUND）。
    virtual Result<void> Cancel(TimerId id) = 0;

    /// 队列中待触发的定时器数量。
    virtual std::size_t Size() const noexcept = 0;

    /// 触发所有 deadline <= now 的定时器，返回本轮触发次数。
    virtual std::uint32_t FireReady(SteadyTime now) = 0;

    /// 距离下一个 deadline 还有多少纳秒；队列为空返回 kNoDeadline。
    virtual SteadyNs NextDeadlineDelayNs(SteadyTime now) const noexcept = 0;
};

}  // namespace mmo::core
