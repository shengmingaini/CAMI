// gateway/ratelimit/ratelimit_selfcheck.cpp
// 确定性 selfcheck：注入假时钟，验证令牌桶 / 连接频率 / 黑白名单 / 门面优先级。
// 无 GTest、无真实 socket、零外部依赖 —— CAMI_BUILD_MODULES=OFF/ON 均编译通过。
#include "gateway/ratelimit/ratelimit_selfcheck.h"
#include "gateway/ratelimit/ratelimit.h"

#include <cstdio>

namespace cami::gateway::ratelimit {

bool ratelimit_selfcheck() {
    bool ok = true;

    // 可注入假时钟：所有组件共享同一 now_ms，便于确定性验证。
    struct FakeClock {
        int64_t now_ms = 0;
    };
    FakeClock clk;
    ClockFn now = [&clk]() -> int64_t { return clk.now_ms; };

    // --- 令牌桶：容量 3，每秒补充 1 ---
    {
        TokenBucket b(/*capacity=*/3.0, /*refill_per_sec=*/1.0, now);
        if (!b.allow() || !b.allow() || !b.allow()) {
            std::fprintf(stderr, "[FAIL] ratelimit: token bucket should allow 3 initial\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: token bucket allows up to capacity (3)\n");
        }
        if (b.allow()) {  // 第 4 次应拒绝
            std::fprintf(stderr, "[FAIL] ratelimit: token bucket should deny when empty\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: token bucket denies when exhausted\n");
        }
        clk.now_ms += 1000;  // 补充 1 令牌
        if (!b.allow()) {
            std::fprintf(stderr, "[FAIL] ratelimit: token bucket should refill after 1s\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: token bucket refills after window\n");
        }
    }

    // --- 连接频率：单窗口最多 2 次 ---
    {
        ConnFreqLimiter f(/*max=*/2, /*window_ms=*/1000, now);
        if (!f.allow() || !f.allow()) {
            std::fprintf(stderr, "[FAIL] ratelimit: conn-freq should allow 2 in window\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: conn-freq allows up to max (2)\n");
        }
        if (f.allow()) {
            std::fprintf(stderr, "[FAIL] ratelimit: conn-freq should deny over max\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: conn-freq denies over max\n");
        }
        clk.now_ms += 1000;  // 窗口滑动
        if (!f.allow()) {
            std::fprintf(stderr, "[FAIL] ratelimit: conn-freq should reset after window\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: conn-freq resets after window\n");
        }
    }

    // --- 黑白名单 ---
    {
        IpList list(now);
        list.ban("10.0.0.1", /*ttl_ms=*/1000);
        if (!list.is_blacklisted("10.0.0.1")) {
            std::fprintf(stderr, "[FAIL] ratelimit: banned ip should be blacklisted\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: banned ip detected (TTL)\n");
        }
        clk.now_ms += 1500;  // 过期
        if (list.is_blacklisted("10.0.0.1")) {
            std::fprintf(stderr, "[FAIL] ratelimit: ban should expire after TTL\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: ban expires after TTL\n");
        }
        list.ban("10.0.0.2", /*ttl_ms=*/0);  // 永久封禁
        if (!list.is_blacklisted("10.0.0.2")) {
            std::fprintf(stderr, "[FAIL] ratelimit: permanent ban (ttl=0) should hold\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: permanent ban (ttl=0) holds\n");
        }
        list.allow("192.168.1.1");
        if (!list.is_whitelisted("192.168.1.1")) {
            std::fprintf(stderr, "[FAIL] ratelimit: whitelisted ip not detected\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: whitelist detected\n");
        }
    }

    // --- 门面优先级：白名单 > 黑名单 > 令牌桶 > 连接频率 ---
    {
        RateLimiterConfig cfg;
        cfg.token_bucket.capacity = 1.0;
        cfg.token_bucket.refill_per_sec = 0.0;  // 不补充，便于触发耗尽
        cfg.conn_freq.max_per_window = 1;
        cfg.conn_freq.window_sec = 1;
        RateLimiter rl(cfg, now);

        // 先把令牌桶与连接频率用掉（同一 IP 计两次请求）
        RateLimitResult r1 = rl.check("1.1.1.1");
        if (r1 != RateLimitResult::kAllow) {
            std::fprintf(stderr, "[FAIL] ratelimit: first check should allow\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: facade allows first request (bucket+freq ok)\n");
        }
        RateLimitResult r2 = rl.check("1.1.1.1");  // 令牌桶或连接频率耗尽 → 拒绝
        if (r2 == RateLimitResult::kAllow) {
            std::fprintf(stderr, "[FAIL] ratelimit: second check should deny (bucket/freq exhausted)\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: facade denies when limits exhausted (result=%d)\n",
                        static_cast<int>(r2));
        }

        // 白名单直通：即使令牌桶已耗尽仍放行
        rl.allow("8.8.8.8");
        RateLimitResult rw = rl.check("8.8.8.8");
        if (rw != RateLimitResult::kAllowWhitelist) {
            std::fprintf(stderr, "[FAIL] ratelimit: whitelisted ip should bypass limits\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: whitelist bypasses limits\n");
        }

        // 黑名单优先于限流：被封 IP 直接拒绝
        rl.ban("2.2.2.2", /*ttl_ms=*/0);
        RateLimitResult rb = rl.check("2.2.2.2");
        if (rb != RateLimitResult::kDenyBlacklist) {
            std::fprintf(stderr, "[FAIL] ratelimit: blacklisted ip should be denied\n");
            ok = false;
        } else {
            std::printf("[ OK ] ratelimit: blacklist denies\n");
        }
    }

    return ok;
}

} // namespace cami::gateway::ratelimit
