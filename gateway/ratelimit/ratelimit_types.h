#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace cami::gateway::ratelimit {

// 单调毫秒时钟；selfcheck/单测注入假时钟以确定性验证，生产默认走 steady_clock。
using ClockFn = std::function<int64_t()>;

enum class RateLimitResult : int {
    kAllow = 0,           // 限流通过
    kAllowWhitelist = 1,  // 白名单直通（快速路径）
    kDenyBlacklist = 2,   // IP 在黑名单
    kDenyTokenBucket = 3, // 令牌桶耗尽
    kDenyConnFreq = 4,    // 连接频率超限
};

struct TokenBucketConfig {
    double capacity = 100.0;       // 桶容量（令牌数）
    double refill_per_sec = 50.0;  // 每秒补充令牌数
};

struct ConnFreqConfig {
    int max_per_window = 20;  // 单窗口内允许的新连接数
    int window_sec = 1;       // 窗口长度（秒）
};

struct RateLimiterConfig {
    TokenBucketConfig token_bucket;
    ConnFreqConfig conn_freq;
};

} // namespace cami::gateway::ratelimit
