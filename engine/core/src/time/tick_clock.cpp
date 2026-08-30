#include "mmo/core/time/tick_clock.h"

#include <chrono>

namespace mmo::core {

TickClock::TickClock(std::uint32_t hz) noexcept
    : hz_(hz == 0 ? 1U : hz),
      interval_ns_(kSteadyNsPerSecond / static_cast<SteadyNs>(hz_)) {
    // 极端 hz（> 1e9）会让间隔退化为 0，导致 Tick 无限补跑；下限截为 1ns。
    if (interval_ns_ < 1) {
        interval_ns_ = 1;
    }
}

DurationMs TickClock::TickInterval() const noexcept {
    return std::chrono::duration_cast<DurationMs>(std::chrono::nanoseconds(interval_ns_));
}

SteadyTime TickClock::NextTickDeadline(SteadyTime prev) const noexcept {
    // 整数纳秒累加 -> 零漂移。等价于 prev + interval_ns_，不读任何时钟。
    return prev + std::chrono::duration_cast<SteadyTime::duration>(
                      std::chrono::nanoseconds(interval_ns_));
}

std::uint32_t TickClock::CatchUpSteps(SteadyTime now, SteadyTime prev) const noexcept {
    const SteadyNs delta = MonotonicClock::Elapsed(prev, now);
    if (delta < interval_ns_) {
        return 0;  // now <= prev 也落在这一支，天然防御时钟回拨
    }
    const auto steps =
        static_cast<std::uint64_t>(delta) / static_cast<std::uint64_t>(interval_ns_);
    if (steps > kMaxCatchUpSteps) {
        return kMaxCatchUpSteps;
    }
    return static_cast<std::uint32_t>(steps);
}

}  // namespace mmo::core
