#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gateway/integration/gateway_pipeline.h"
#include "gateway/ratelimit/ratelimit.h"
#include "gateway/ratelimit/ratelimit_types.h"
#include "gateway/security/token_auth.h"
#include "gateway/router/router.h"
#include "gateway/redis/in_memory_state.h"

using namespace cami::gateway;

namespace {

// 确定性假时钟：固定值，不推进（本测试不依赖时间）。
ratelimit::ClockFn FakeClock() {
    return []() -> int64_t { return 1'000'000; };
}

// 构造一个带 4 个 game 后端的集成管线（测试夹具复用）。
integration::GatewayPipeline MakePipeline() {
    ratelimit::RateLimiterConfig cfg;
    security::TokenAuth auth(3600 * 1000);
    router::Router router(router::RouterConfig{});
    router.reload_backends({"game0", "game1", "game2", "game3"});
    auto store = std::make_shared<redis::InMemoryState>(FakeClock(), 0);
    return integration::GatewayPipeline(cfg, std::move(auth), std::move(router),
                                        std::move(store), FakeClock());
}

}  // namespace

TEST(GatewayPipeline, OnAcceptBlacklistDenied) {
    auto pipe = MakePipeline();
    pipe.ban("203.0.113.9", 60'000);
    EXPECT_EQ(pipe.on_accept("203.0.113.9"),
              ratelimit::RateLimitResult::kDenyBlacklist);
}

TEST(GatewayPipeline, OnAcceptWhitelistAllowed) {
    auto pipe = MakePipeline();
    pipe.allow("198.51.100.7");
    EXPECT_EQ(pipe.on_accept("198.51.100.7"),
              ratelimit::RateLimitResult::kAllowWhitelist);
}

TEST(GatewayPipeline, OnAcceptNormalAllowed) {
    auto pipe = MakePipeline();
    EXPECT_EQ(pipe.on_accept("192.0.2.50"),
              ratelimit::RateLimitResult::kAllow);
}

TEST(GatewayPipeline, AuthenticateValidAndBogus) {
    // issue 必须在移动 TokenAuth 进 pipeline 之前。
    security::TokenAuth auth(3600 * 1000);
    const std::string tok = auth.issue(12345);
    ratelimit::RateLimiterConfig cfg;
    router::Router router(router::RouterConfig{});
    router.reload_backends({"game0"});
    auto store = std::make_shared<redis::InMemoryState>(FakeClock(), 0);
    integration::GatewayPipeline pipe(cfg, std::move(auth), std::move(router),
                                      std::move(store), FakeClock());

    std::uint64_t pid = 0;
    EXPECT_TRUE(pipe.authenticate(tok, pid));
    EXPECT_EQ(pid, 12345u);

    std::uint64_t pid2 = 0;
    EXPECT_FALSE(pipe.authenticate("deadbeef", pid2));
}

TEST(GatewayPipeline, RouteBackendDeterministic) {
    auto pipe = MakePipeline();
    const std::string b1 = pipe.route_backend(777);
    const std::string b2 = pipe.route_backend(777);
    const std::string b3 = pipe.route_backend(888);
    EXPECT_FALSE(b1.empty());
    EXPECT_EQ(b1, b2);          // 同 player_id 命中同后端
    EXPECT_FALSE(b3.empty());
}

TEST(GatewayPipeline, OnlineStateRoundTrip) {
    auto pipe = MakePipeline();
    pipe.on_session_established(4242, "game0");
    ASSERT_NE(pipe.get_store(), nullptr);
    EXPECT_TRUE(pipe.get_store()->is_online(4242));
    EXPECT_EQ(pipe.get_store()->get_backend(4242), "game0");

    pipe.on_session_closed(4242);
    EXPECT_FALSE(pipe.get_store()->is_online(4242));
}
