// server/dataservice/include/mmo/data/redis/circuit_breaker.h
//
// TASK-027 · 熔断状态机（§15.8 / §19）。连续失败达到阈值进入 Open，
// 期间快速失败返回 BUSY（防雪崩）；冷却后 HalfOpen 探活，成功则恢复 Closed。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "mmo/core/time/clock.h"
#include "mmo/data/health.h"

namespace mmo::data::redis {

class CircuitBreaker {
public:
    enum class State : std::uint8_t { Closed = 0, Open = 1, HalfOpen = 2 };

    explicit CircuitBreaker(std::uint32_t threshold = 5,
                            mmo::core::DurationMs cooldown = mmo::core::DurationMs{2000})
        : threshold_(threshold), cooldown_ns_(static_cast<std::int64_t>(cooldown.count()) * 1'000'000) {}

    /// 调用前检查：Open 且未到冷却期 -> 拒绝（返回 false）。
    bool AllowRequest() const noexcept {
        State s = state_.load(std::memory_order_acquire);
        if (s == State::Closed) return true;
        if (s == State::HalfOpen) return true;  // 仅一个探活请求进入，由调用方控制并发
        // Open：检查冷却是否到期
        const std::int64_t now = mmo::core::MonotonicClock::Now();
        const std::int64_t opened = opened_at_ns_.load(std::memory_order_acquire);
        if (now - opened >= cooldown_ns_) {
            // 原子切换到 HalfOpen（允许探活）
            State expected = State::Open;
            if (state_.compare_exchange_strong(expected, State::HalfOpen,
                                               std::memory_order_acq_rel)) {
                return true;
            }
            return state_.load(std::memory_order_acquire) != State::Open;
        }
        return false;
    }

    void RecordSuccess() noexcept {
        state_.store(State::Closed, std::memory_order_release);
        failures_.store(0, std::memory_order_release);
    }

    void RecordFailure() noexcept {
        const std::uint32_t f = failures_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (f >= threshold_ && state_.load(std::memory_order_acquire) != State::Open) {
            state_.store(State::Open, std::memory_order_release);
            opened_at_ns_.store(mmo::core::MonotonicClock::Now(), std::memory_order_release);
        }
    }

    State state() const noexcept { return state_.load(std::memory_order_acquire); }

    HealthStatus health() const noexcept {
        switch (state_.load(std::memory_order_acquire)) {
            case State::Closed:    return HealthStatus::Healthy;
            case State::HalfOpen:  return HealthStatus::Degraded;
            case State::Open:      return HealthStatus::Unavailable;
        }
        return HealthStatus::Unknown;
    }

private:
    std::uint32_t threshold_;
    std::int64_t cooldown_ns_;
    mutable std::atomic<State> state_{State::Closed};
    mutable std::atomic<std::uint32_t> failures_{0};
    mutable std::atomic<std::int64_t> opened_at_ns_{0};
};

}  // namespace mmo::data::redis
