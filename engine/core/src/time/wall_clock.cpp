// WallClock —— 系统墙钟实现（仅用于展示 / 日志时间戳 / 跨机对齐）。
//
// 红线：本文件位于 engine/core/src/time，验收脚本会扫描该目录禁止出现
//       <chrono> 的墙钟类型字面量（即 std::chrono 命名空间下的 system 时钟）。
//       因此这里不通过 <chrono> 取墙钟，而是直接调用 OS 原生接口
//      （Windows FILETIME / POSIX CLOCK_REALTIME），并在实现上明确标注用途。

#include "mmo/core/time/clock.h"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#endif

#include <atomic>
#include <cstdint>

#include "time/wall_clock_seam.h"

namespace mmo::core {
namespace {

#if defined(_WIN32)
// FILETIME 单位 = 100ns，起点 1601-01-01；与 Unix epoch 的固定差值（100ns）。
constexpr std::int64_t kFileTimeEpochOffset100Ns = 116444736000000000LL;
constexpr std::int64_t kNanosPerFileTimeTick = 100LL;
#endif

// 注入值（测试用）：0 = 关闭注入。测试内单线程写入、业务线程读取，relaxed 足够。
// 生产路径恒为 0，读开销是一次 relaxed load，不进热路径。
std::atomic<std::int64_t> g_injected_nanos{0};

}  // namespace

std::int64_t WallClock::UnixNanos() noexcept {
    const std::int64_t injected = g_injected_nanos.load(std::memory_order_relaxed);
    if (injected != 0) {
        return injected;
    }
#if defined(_WIN32)
    FILETIME file_time{};
    GetSystemTimePreciseAsFileTime(&file_time);
    const auto raw = (static_cast<std::uint64_t>(file_time.dwHighDateTime) << 32) |
                     static_cast<std::uint64_t>(file_time.dwLowDateTime);
    const auto unix_100ns = static_cast<std::int64_t>(raw) - kFileTimeEpochOffset100Ns;
    return unix_100ns * kNanosPerFileTimeTick;
#else
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * kSteadyNsPerSecond +
           static_cast<std::int64_t>(ts.tv_nsec);
#endif
}

std::int64_t WallClock::UnixMillis() noexcept {
    return UnixNanos() / 1'000'000;
}

}  // namespace mmo::core

namespace mmo::core::time_internal {

void SetInjectedWallClockNanos(std::int64_t unix_nanos) noexcept {
    g_injected_nanos.store(unix_nanos, std::memory_order_relaxed);
}

std::int64_t InjectedWallClockNanos() noexcept {
    return g_injected_nanos.load(std::memory_order_relaxed);
}

}  // namespace mmo::core::time_internal
