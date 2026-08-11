// gateway/ratelimit/ratelimit.cpp
// 限流防攻击核心实现：每源 IP 令牌桶 + 连接频率限制 + 黑白名单。
//
// 设计约定：
// - 纯内存、无 socket 归属、零外部依赖；transport-agnostic，网关连接层在
//   accept / on_data 时调用 RateLimiter::check(ip)。
// - 时钟可注入（ClockFn）；selfcheck 与单测注入假时钟以确定性验证，
//   生产默认走 steady_clock。
// - 优先级：白名单 > 黑名单 > 令牌桶 > 连接频率（见 RateLimiter::check）。

#include "gateway/ratelimit/ratelimit.h"

#include <algorithm>
#include <cstdint>

namespace cami::gateway::ratelimit {

// === TokenBucket ===
TokenBucket::TokenBucket(double capacity, double refill_per_sec, ClockFn now)
    : capacity_(capacity),
      refill_(refill_per_sec),
      tokens_(capacity),  // 初始满桶
      last_ms_(now()),
      now_(now) {}

void TokenBucket::refill() {
    const int64_t t = now_();
    const double elapsed_sec = static_cast<double>(t - last_ms_) / 1000.0;
    if (elapsed_sec > 0.0) {
        tokens_ = std::min(capacity_, tokens_ + elapsed_sec * refill_);
    }
    last_ms_ = t;
}

bool TokenBucket::allow(int64_t tokens) {
    refill();
    if (tokens > static_cast<int64_t>(capacity_)) {
        return false;  // 单请求超过桶容量，永不可满足
    }
    if (tokens_ >= static_cast<double>(tokens)) {
        tokens_ -= static_cast<double>(tokens);
        return true;
    }
    return false;
}

// === ConnFreqLimiter ===
ConnFreqLimiter::ConnFreqLimiter(int max_per_window, int64_t window_ms, ClockFn now)
    : max_(max_per_window),
      window_ms_(window_ms),
      now_(now),
      count_(0),
      window_start_ms_(now()) {}

bool ConnFreqLimiter::allow() {
    const int64_t t = now_();
    if (t >= window_start_ms_ + window_ms_) {
        window_start_ms_ = t;  // 窗口滑动后重置
        count_ = 0;
    }
    ++count_;
    return count_ <= max_;
}

// === IpList ===
IpList::IpList(ClockFn now) : now_(now) {}

void IpList::ban(const std::string& ip, int64_t ttl_ms) {
    // ttl_ms <= 0 表示永久封禁，存 0 作为哨兵。
    blacklist_[ip] = (ttl_ms <= 0) ? 0 : (now_() + ttl_ms);
}

void IpList::unban(const std::string& ip) { blacklist_.erase(ip); }

void IpList::allow(const std::string& ip) { whitelist_.insert(ip); }

bool IpList::is_whitelisted(const std::string& ip) const {
    return whitelist_.find(ip) != whitelist_.end();
}

bool IpList::is_blacklisted(const std::string& ip) const {
    auto it = blacklist_.find(ip);
    if (it == blacklist_.end()) return false;
    if (it->second == 0) return true;       // 永久封禁
    if (now_() >= it->second) return false;  // 已过期（物理清理由 prune() 完成）
    return true;
}

void IpList::prune() {
    const int64_t t = now_();
    for (auto it = blacklist_.begin(); it != blacklist_.end();) {
        if (it->second != 0 && t >= it->second) {
            it = blacklist_.erase(it);
        } else {
            ++it;
        }
    }
}

// === RateLimiter ===
RateLimiter::RateLimiter(const RateLimiterConfig& cfg, ClockFn now)
    : cfg_(cfg), iplist_(now), now_(now) {}

TokenBucket& RateLimiter::bucket_for(const std::string& ip) {
    auto it = buckets_.find(ip);
    if (it != buckets_.end()) return it->second;
    return buckets_
        .emplace(ip, TokenBucket(cfg_.token_bucket.capacity,
                                 cfg_.token_bucket.refill_per_sec, now_))
        .first->second;
}

ConnFreqLimiter& RateLimiter::freq_for(const std::string& ip) {
    auto it = freqs_.find(ip);
    if (it != freqs_.end()) return it->second;
    return freqs_
        .emplace(ip, ConnFreqLimiter(cfg_.conn_freq.max_per_window,
                                     static_cast<int64_t>(cfg_.conn_freq.window_sec) * 1000,
                                     now_))
        .first->second;
}

RateLimitResult RateLimiter::check(const std::string& ip) {
    if (iplist_.is_whitelisted(ip)) return RateLimitResult::kAllowWhitelist;
    if (iplist_.is_blacklisted(ip)) return RateLimitResult::kDenyBlacklist;
    if (!bucket_for(ip).allow()) return RateLimitResult::kDenyTokenBucket;
    if (!freq_for(ip).allow()) return RateLimitResult::kDenyConnFreq;
    return RateLimitResult::kAllow;
}

void RateLimiter::ban(const std::string& ip, int64_t ttl_ms) { iplist_.ban(ip, ttl_ms); }

void RateLimiter::allow(const std::string& ip) { iplist_.allow(ip); }

} // namespace cami::gateway::ratelimit
