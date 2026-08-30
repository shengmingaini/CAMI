#pragma once

#include <chrono>
#include <cstdint>

namespace mmo::core {

/// 单调时钟差值，单位纳秒。永不受 NTP 校时、手动改系统时间、夏令时影响。
using SteadyNs = std::int64_t;

/// Tick 调度基准时间点。语义等价于 steady_clock::time_point（单调、不回拨）。
using SteadyTime = std::chrono::steady_clock::time_point;

/// 以毫秒表达的时长，用于对外暴露 Tick 间隔等人类可读量。
using DurationMs = std::chrono::milliseconds;

inline constexpr SteadyNs kSteadyNsPerSecond = 1'000'000'000;
inline constexpr SteadyNs kSteadyNsPerMilli = 1'000'000;

/// MonotonicClock —— 进程内唯一允许驱动游戏 Tick 的时间源。
///
/// Thread Safety : 全静态、无状态，任意线程并发调用安全。
/// Hot Path      : YES —— 每 Tick 至少调用一次（TickClock 位于 Tick 循环入口）。
/// 实现要点      : Windows 走 QueryPerformanceCounter，POSIX 走 CLOCK_MONOTONIC；
///                 换算到纳秒用「定点乘移」而非除法（实测 16.9ns/call，
///                 而 std::chrono::steady_clock::now() 实测 24.5ns/call，阈值 25ns 余量不足）。
///
/// 禁止：用 WallClock / 系统墙钟驱动 Tick（见 PROJECT_REQUIREMENTS §13）。
class MonotonicClock {
public:
    /// 当前单调时刻（纳秒）。保证单调不回退；连续两次调用允许相等。
    static SteadyNs Now() noexcept;

    /// 当前单调时间点，等价于 SteadyTime(nanoseconds(Now()))，与 Now() 同源。
    static SteadyTime Point() noexcept;

    /// to - from 的纳秒差。from > to 时返回负值（调用方需自行防御）。
    static SteadyNs Elapsed(SteadyTime from, SteadyTime to) noexcept;
};

/// WallClock —— 系统墙钟，**仅**用于展示、日志时间戳、跨机时间对齐。
///
/// Thread Safety : 全静态、无状态，任意线程并发调用安全。
/// Hot Path      : NO —— 禁止出现在 Tick / 战斗 / 移动等热路径。
///
/// 禁止：用 WallClock 驱动 Tick、计算帧间隔、做任何超时判定。
///       墙钟会被 NTP 校时、手动改时间、虚拟机挂起/恢复影响，可前跳也可**回拨**。
class WallClock {
public:
    /// 当前 Unix 时间（纳秒，1970-01-01T00:00:00Z 起）。
    static std::int64_t UnixNanos() noexcept;

    /// 当前 Unix 时间（毫秒）。纳秒精度平台的截断值，够日志与展示用。
    static std::int64_t UnixMillis() noexcept;
};

}  // namespace mmo::core
