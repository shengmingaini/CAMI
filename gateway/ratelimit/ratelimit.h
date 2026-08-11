#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "gateway/ratelimit/ratelimit_types.h"

namespace cami::gateway::ratelimit {

// 令牌桶：容量 + 恒定速率补充；allow() 在有余量时消费并返回 true。
class TokenBucket {
public:
    TokenBucket(double capacity, double refill_per_sec, ClockFn now);
    bool allow(int64_t tokens = 1);
    double tokens() const { return tokens_; }
private:
    void refill();
    double capacity_, refill_, tokens_;
    int64_t last_ms_;
    ClockFn now_;
};

// 连接频率限制：固定窗口内允许有限次新连接；窗口滑动后重置。
class ConnFreqLimiter {
public:
    ConnFreqLimiter(int max_per_window, int64_t window_ms, ClockFn now);
    bool allow();
private:
    int max_;
    int64_t window_ms_;
    ClockFn now_;
    int count_ = 0;
    int64_t window_start_ms_;
};

// IP 黑白名单：白名单永久直通；黑名单支持 TTL 过期（TTL<=0 表示永久封禁）。
class IpList {
public:
    explicit IpList(ClockFn now);
    void ban(const std::string& ip, int64_t ttl_ms);
    void unban(const std::string& ip);
    void allow(const std::string& ip);  // 加入白名单（永久）
    bool is_whitelisted(const std::string& ip) const;
    bool is_blacklisted(const std::string& ip) const;
    void prune();  // 清理过期黑名单项
private:
    std::unordered_map<std::string, int64_t> blacklist_;  // ip -> 过期毫秒(0=永久)
    std::unordered_set<std::string> whitelist_;
    ClockFn now_;
};

// 限流门面：按源 IP 组合 白名单 > 黑名单 > 令牌桶 > 连接频率。
// 纯内存、无 socket 归属；网关连接层在 accept/on_data 时调用 check(ip)。
class RateLimiter {
public:
    explicit RateLimiter(const RateLimiterConfig& cfg, ClockFn now);
    RateLimitResult check(const std::string& ip);
    void ban(const std::string& ip, int64_t ttl_ms);
    void allow(const std::string& ip);
private:
    TokenBucket& bucket_for(const std::string& ip);
    ConnFreqLimiter& freq_for(const std::string& ip);
    RateLimiterConfig cfg_;
    IpList iplist_;
    ClockFn now_;
    std::unordered_map<std::string, TokenBucket> buckets_;
    std::unordered_map<std::string, ConnFreqLimiter> freqs_;
};

} // namespace cami::gateway::ratelimit
