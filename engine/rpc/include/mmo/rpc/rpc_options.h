// rpc_options.h — 每次调用必填的 RPC 选项（TASK-006 §7）
//
// 红线：
//   - 非幂等调用禁止自动重试（idempotent=false 时重试次数恒为 0）；
//   - 退避必须指数 + 抖动 + 上限（1s），禁止固定间隔/无限重试；
//   - timeout_ms=0 视为配置错误，返回 INVALID_ARGUMENT，禁止"永不超时"。
#pragma once

#include <cstdint>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"

namespace mmo::rpc {

struct RpcOptions {
    /// 单次尝试的超时（重试的每次尝试各自获得全新 deadline）。
    core::DurationMs timeout_ms{500};
    bool     retry_on_unavailable{true};
    uint32_t max_retries{2};
    /// 指数退避基准：第 n 次重试前等待 base << n，带 ±20% 抖动，上限 1s。
    core::DurationMs backoff_base_ms{20};
    /// 非幂等调用禁止自动重试（默认 false = 不重试）。
    bool idempotent{false};
};

/// 指数退避上限（§19 重试风暴保护）。
inline constexpr core::DurationMs kBackoffCapMs{1000};

/// 校验 RpcOptions：timeout_ms 必须 > 0。
core::Error ValidateRpcOptions(const RpcOptions& opts) noexcept;

/// 第 attempt 次重试（attempt 从 0 计）前的退避时长：
/// min(backoff_base << attempt, 1s) * jitter，jitter ∈ [0.8, 1.2]。
/// jitter_seed 任意（测试可传固定值保证可复现；生产传随机数）。
core::DurationMs ComputeBackoff(uint32_t attempt, const RpcOptions& opts,
                                uint32_t jitter_seed) noexcept;

/// 重试判定：仅当幂等且错误码 ∈ {TIMEOUT, BUSY, RATE_LIMITED}
///（即 gRPC 的 DEADLINE_EXCEEDED / UNAVAILABLE / RESOURCE_EXHAUSTED）才重试。
bool ShouldRetry(const RpcOptions& opts, core::ErrorCode code) noexcept;

}  // namespace mmo::rpc
