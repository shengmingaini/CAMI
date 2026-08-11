#include <gtest/gtest.h>

#include "gateway/ratelimit/ratelimit.h"

using namespace cami::gateway::ratelimit;

// 确定性假时钟：selfcheck 与单测共享，避免 sleep / 真实时间抖动。
struct FakeClock {
    int64_t now_ms = 0;
};

TEST(TokenBucket, ExhaustThenRefill) {
    FakeClock clk;
    ClockFn now = [&clk]() -> int64_t { return clk.now_ms; };
    TokenBucket b(/*capacity=*/3.0, /*refill_per_sec=*/1.0, now);
    EXPECT_TRUE(b.allow());
    EXPECT_TRUE(b.allow());
    EXPECT_TRUE(b.allow());
    EXPECT_FALSE(b.allow());  // 桶空，拒绝
    clk.now_ms += 1000;       // 补充 1 令牌
    EXPECT_TRUE(b.allow());
    EXPECT_FALSE(b.allow());  // 再次空
}

TEST(TokenBucket, OverCapacityRequestNeverServes) {
    FakeClock clk;
    ClockFn now = [&clk]() -> int64_t { return clk.now_ms; };
    TokenBucket b(/*capacity=*/2.0, /*refill_per_sec=*/0.0, now);
    EXPECT_FALSE(b.allow(/*tokens=*/5));  // 单请求超过容量
}

TEST(ConnFreqLimiter, WindowReset) {
    FakeClock clk;
    ClockFn now = [&clk]() -> int64_t { return clk.now_ms; };
    ConnFreqLimiter f(/*max=*/2, /*window_ms=*/1000, now);
    EXPECT_TRUE(f.allow());
    EXPECT_TRUE(f.allow());
    EXPECT_FALSE(f.allow());  // 超过窗口上限
    clk.now_ms += 1000;       // 窗口滑动
    EXPECT_TRUE(f.allow());
}

TEST(IpList, TtlAndPermanent) {
    FakeClock clk;
    ClockFn now = [&clk]() -> int64_t { return clk.now_ms; };
    IpList list(now);
    list.ban("1.2.3.4", /*ttl_ms=*/1000);
    EXPECT_TRUE(list.is_blacklisted("1.2.3.4"));
    clk.now_ms += 1500;
    EXPECT_FALSE(list.is_blacklisted("1.2.3.4"));  // 过期
    list.ban("5.6.7.8", /*ttl_ms=*/0);             // 永久封禁
    EXPECT_TRUE(list.is_blacklisted("5.6.7.8"));
    list.unban("5.6.7.8");
    EXPECT_FALSE(list.is_blacklisted("5.6.7.8"));
    list.allow("9.9.9.9");
    EXPECT_TRUE(list.is_whitelisted("9.9.9.9"));
}

TEST(RateLimiter, PriorityWhitelistBlacklist) {
    FakeClock clk;
    ClockFn now = [&clk]() -> int64_t { return clk.now_ms; };
    RateLimiterConfig cfg;
    cfg.token_bucket.capacity = 1.0;
    cfg.token_bucket.refill_per_sec = 0.0;  // 不补充，便于触发耗尽
    cfg.conn_freq.max_per_window = 1;
    cfg.conn_freq.window_sec = 1;
    RateLimiter rl(cfg, now);

    rl.allow("8.8.8.8");
    EXPECT_EQ(rl.check("8.8.8.8"), RateLimitResult::kAllowWhitelist);

    EXPECT_EQ(rl.check("1.1.1.1"), RateLimitResult::kAllow);
    EXPECT_NE(rl.check("1.1.1.1"), RateLimitResult::kAllow);  // 桶/频率耗尽

    rl.ban("2.2.2.2", /*ttl_ms=*/0);
    EXPECT_EQ(rl.check("2.2.2.2"), RateLimitResult::kDenyBlacklist);
}
