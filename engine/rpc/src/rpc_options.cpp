// grpc_options.cpp — RpcOptions 校验 / 退避 / 重试判定（TASK-006 §15.3-4）
#include "mmo/rpc/rpc_options.h"

#include <thread>

namespace mmo::rpc {

using core::DurationMs;
using core::ErrorCode;

core::Error ValidateRpcOptions(const RpcOptions& opts) noexcept {
    if (opts.timeout_ms <= DurationMs::zero()) {
        return core::Error{ErrorCode::INVALID_ARGUMENT,
                           "rpc: timeout_ms must be > 0", core::domain::kNet};
    }
    return core::Error{ErrorCode::OK, "ok", core::domain::kNet};
}

DurationMs ComputeBackoff(uint32_t attempt, const RpcOptions& opts,
                          uint32_t jitter_seed) noexcept {
    // base << attempt，饱和左移，上限 1s。
    int64_t ms = opts.backoff_base_ms.count();
    for (uint32_t i = 0; i < attempt && ms < kBackoffCapMs.count(); ++i) {
        ms *= 2;
    }
    if (ms > kBackoffCapMs.count()) ms = kBackoffCapMs.count();

    // 抖动 ±20%：jitter ∈ [0.8, 1.2]（jitter_seed 任意，如随机数/键哈希）。
    constexpr int64_t kJitterDenom = 1000;
    const int64_t num = 800 + (jitter_seed % 401);  // [800, 1200]
    ms = ms * num / kJitterDenom;
    if (ms < 1) ms = 1;
    return DurationMs{ms};
}

bool ShouldRetry(const RpcOptions& opts, ErrorCode code) noexcept {
    if (!opts.idempotent || !opts.retry_on_unavailable) return false;  // 红线：非幂等禁重试
    switch (code) {
        case ErrorCode::TIMEOUT:        // ← DEADLINE_EXCEEDED
        case ErrorCode::BUSY:           // ← UNAVAILABLE
        case ErrorCode::RATE_LIMITED:   // ← RESOURCE_EXHAUSTED
            return true;
        default:
            return false;
    }
}

}  // namespace mmo::rpc
