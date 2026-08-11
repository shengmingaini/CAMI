#include "gateway/integration/gateway_pipeline_selfcheck.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "gateway/integration/gateway_pipeline.h"
#include "gateway/ratelimit/ratelimit.h"        // RateLimiter, RateLimitResult
#include "gateway/ratelimit/ratelimit_types.h"  // ClockFn, RateLimiterConfig
#include "gateway/security/token_auth.h"        // TokenAuth
#include "gateway/security/security_types.h"    // SecResult
#include "gateway/router/router.h"              // Router, RouterConfig
#include "gateway/redis/online_state.h"         // OnlineStateStore
#include "gateway/redis/in_memory_state.h"      // InMemoryState

namespace cami {
namespace gateway {
namespace integration {

bool gateway_pipeline_selfcheck() {
    bool ok = true;

    // 确定性假时钟（固定值即可；限流/在线态本自检不依赖时间推进）。
    ratelimit::ClockFn now = []() -> int64_t { return 1'000'000; };

    // --- 缝① 限流：黑白名单优先级 ---
    {
        ratelimit::RateLimiterConfig cfg;
        security::TokenAuth auth(/*ttl_ms=*/3600 * 1000);
        router::Router router(router::RouterConfig{});
        router.reload_backends({"game0", "game1", "game2"});
        auto store = std::make_shared<redis::InMemoryState>(now, /*ttl_ms=*/0);

        GatewayPipeline pipe(cfg, std::move(auth), std::move(router),
                             std::move(store), now);

        const std::string bad_ip = "203.0.113.9";
        pipe.ban(bad_ip, /*ttl_ms=*/60'000);  // 经 RateLimiter 暴露的白名单/黑名单接口
        const auto r_black = pipe.on_accept(bad_ip);
        if (r_black != ratelimit::RateLimitResult::kDenyBlacklist) {
            std::fprintf(stderr,
                         "[FAIL] integration: blacklisted IP not denied (got %d)\n",
                         static_cast<int>(r_black));
            ok = false;
        } else {
            std::printf("[ OK ] integration: blacklisted IP denied\n");
        }

        const std::string good_ip = "198.51.100.7";
        pipe.allow(good_ip);  // 白名单
        const auto r_white = pipe.on_accept(good_ip);
        if (r_white != ratelimit::RateLimitResult::kAllowWhitelist) {
            std::fprintf(stderr,
                         "[FAIL] integration: whitelisted IP not allowed (got %d)\n",
                         static_cast<int>(r_white));
            ok = false;
        } else {
            std::printf("[ OK ] integration: whitelisted IP allowed\n");
        }

        const std::string normal_ip = "192.0.2.50";
        const auto r_normal = pipe.on_accept(normal_ip);
        if (r_normal != ratelimit::RateLimitResult::kAllow) {
            std::fprintf(stderr,
                         "[FAIL] integration: normal IP not allowed (got %d)\n",
                         static_cast<int>(r_normal));
            ok = false;
        } else {
            std::printf("[ OK ] integration: normal IP allowed\n");
        }
    }

    // --- 缝② 鉴权：合法 token 解析 player_id；非法 token 拒绝 ---
    {
        ratelimit::RateLimiterConfig cfg;
        security::TokenAuth auth(/*ttl_ms=*/3600 * 1000);
        const std::string tok = auth.issue(12345);  // 先在移动前签发
        router::Router router(router::RouterConfig{});
        router.reload_backends({"game0", "game1"});
        auto store = std::make_shared<redis::InMemoryState>(now, 0);

        GatewayPipeline pipe(cfg, std::move(auth), std::move(router),
                             std::move(store), now);

        std::uint64_t pid = 0;
        if (!pipe.authenticate(tok, pid) || pid != 12345) {
            std::fprintf(stderr,
                         "[FAIL] integration: valid token not authenticated\n");
            ok = false;
        } else {
            std::printf("[ OK ] integration: valid token -> player_id=%llu\n",
                        static_cast<unsigned long long>(pid));
        }

        std::uint64_t pid2 = 0;
        if (pipe.authenticate("deadbeef", pid2)) {
            std::fprintf(stderr,
                         "[FAIL] integration: bogus token wrongly accepted\n");
            ok = false;
        } else {
            std::printf("[ OK ] integration: bogus token rejected\n");
        }
    }

    // --- 缝③ 选后端：确定性（同 player_id 命中同后端）---
    {
        ratelimit::RateLimiterConfig cfg;
        security::TokenAuth auth(3600 * 1000);
        router::Router router(router::RouterConfig{});
        router.reload_backends({"game0", "game1", "game2", "game3"});
        auto store = std::make_shared<redis::InMemoryState>(now, 0);

        GatewayPipeline pipe(cfg, std::move(auth), std::move(router),
                             std::move(store), now);

        const std::string b1 = pipe.route_backend(777);
        const std::string b2 = pipe.route_backend(777);
        const std::string b3 = pipe.route_backend(888);
        if (b1.empty() || b1 != b2) {
            std::fprintf(stderr,
                         "[FAIL] integration: route_backend not deterministic\n");
            ok = false;
        } else {
            std::printf("[ OK ] integration: route deterministic (%s)\n", b1.c_str());
        }
        (void)b3;
    }

    // --- 缝④ 在线态：建立/下线 ---
    {
        ratelimit::RateLimiterConfig cfg;
        security::TokenAuth auth(3600 * 1000);
        router::Router router(router::RouterConfig{});
        router.reload_backends({"game0"});
        auto store = std::make_shared<redis::InMemoryState>(now, 0);

        GatewayPipeline pipe(cfg, std::move(auth), std::move(router),
                             std::move(store), now);

        pipe.on_session_established(4242, "game0");
        if (!pipe.get_store()->is_online(4242) ||
            pipe.get_store()->get_backend(4242) != "game0") {
            std::fprintf(stderr,
                         "[FAIL] integration: session not recorded online\n");
            ok = false;
        } else {
            std::printf("[ OK ] integration: session online @ game0\n");
        }

        pipe.on_session_closed(4242);
        if (pipe.get_store()->is_online(4242)) {
            std::fprintf(stderr,
                         "[FAIL] integration: session not cleared offline\n");
            ok = false;
        } else {
            std::printf("[ OK ] integration: session cleared offline\n");
        }
    }

    return ok;
}

}  // namespace integration
}  // namespace gateway
}  // namespace cami
