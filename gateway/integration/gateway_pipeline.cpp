#include "gateway/integration/gateway_pipeline.h"

#include <string>
#include <utility>

#include "gateway/connection/connection_manager.h"  // ConnectionManager, shared_ptr<Connection>
#include "gateway/ratelimit/ratelimit.h"            // RateLimiter, RateLimitResult
#include "gateway/security/token_auth.h"            // TokenAuth
#include "gateway/security/security_types.h"        // SecResult, TokenVerifyResult
#include "gateway/router/router.h"                  // Router
#include "gateway/redis/online_state.h"             // OnlineStateStore

namespace cami {
namespace gateway {
namespace integration {

GatewayPipeline::GatewayPipeline(const ratelimit::RateLimiterConfig& rl_cfg,
                                 security::TokenAuth auth,
                                 router::Router router,
                                 std::shared_ptr<redis::OnlineStateStore> store,
                                 ratelimit::ClockFn now)
    : rl_(rl_cfg, now),
      auth_(std::move(auth)),
      router_(std::move(router)),
      store_(std::move(store)),
      now_(now) {}

ratelimit::RateLimitResult GatewayPipeline::on_accept(const std::string& peer_ip) {
    return rl_.check(peer_ip);
}

bool GatewayPipeline::authenticate(const std::string& token,
                                   std::uint64_t& player_id_out) {
    const security::TokenVerifyResult v = auth_.verify(token);
    if (v.status == security::SecResult::kOk) {
        player_id_out = v.player_id;
        return true;
    }
    return false;
}

std::string GatewayPipeline::route_backend(std::uint64_t player_id) const {
    return router_.route(std::to_string(player_id));
}

void GatewayPipeline::on_session_established(std::uint64_t player_id,
                                             const std::string& backend) {
    if (store_) store_->set_online(player_id, backend);
}

void GatewayPipeline::on_session_closed(std::uint64_t player_id) {
    if (store_) store_->set_offline(player_id);
}

void GatewayPipeline::ban(const std::string& ip, std::int64_t ttl_ms) {
    rl_.ban(ip, ttl_ms);
}

void GatewayPipeline::allow(const std::string& ip) {
    rl_.allow(ip);
}

void GatewayPipeline::install(connection::ConnectionManager& mgr) {
    // 缝①：新连接接入即限流；deny 类结果（黑名单/桶耗尽/频率超限）直接断连。
    mgr.set_on_accept([this](std::shared_ptr<connection::Connection> conn) {
        const auto r = on_accept(conn->peer_address());
        if (r != ratelimit::RateLimitResult::kAllow &&
            r != ratelimit::RateLimitResult::kAllowWhitelist) {
            conn->close();
        }
    });
    // 缝②/③/④ 由上层登录/迁移流程显式调用 authenticate / route_backend /
    // on_session_established / on_session_closed（Connection 不携带 player_id，
    // 会话<->连接 映射属于应用层，不在本集成层内）。
}

}  // namespace integration
}  // namespace gateway
}  // namespace cami
