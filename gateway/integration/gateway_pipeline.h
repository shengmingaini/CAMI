#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "gateway/ratelimit/ratelimit.h"  // RateLimiter, RateLimiterConfig, RateLimitResult, ClockFn
#include "gateway/security/token_auth.h"  // TokenAuth
#include "gateway/router/router.h"        // Router, RouterConfig

// 前置声明：ConnectionManager / Connection 仅在 install() 以引用出现，
// OnlineStateStore 仅以 shared_ptr 出现，无需在头文件引入重型 boost 依赖。
namespace cami {
namespace gateway {
namespace connection {
class ConnectionManager;
class Connection;
}  // namespace connection
namespace redis {
class OnlineStateStore;
}  // namespace redis
}  // namespace gateway
}  // namespace cami

namespace cami {
namespace gateway {
namespace integration {

// GatewayPipeline —— Week3 四模块"集成缝"的组合策略层。
//
// 设计边界（严守"后期模块不 mutate 早期模块"纪律）：
//  - 不修改 connection.cpp / connection_manager.cpp 任何逻辑；
//  - 仅通过 ConnectionManager 既有的 set_on_accept / set_on_connection_closed 挂钩接入；
//  - 四道缝对应方法均为纯函数式策略，可脱离 socket 单测（见 gateway_pipeline_test.cpp）。
//
// 缝映射（详见 docs/design/connection-migration.md §3）：
//  ① 限流(ratelimit)  -> on_accept(peer_ip)              ：deny 类结果由 install() 直接 close()
//  ② 鉴权(security)   -> authenticate(token, player_id) ：首包解析出 token 后调用，失败断连
//  ③ 选后端(router)   -> route_backend(player_id)       ：登录/迁移后选 game 后端
//  ④ 在线态(redis)    -> on_session_established/closed    ：会话建立/下线维护落点
class GatewayPipeline {
public:
    GatewayPipeline(const ratelimit::RateLimiterConfig& rl_cfg,
                    security::TokenAuth auth,
                    router::Router router,
                    std::shared_ptr<redis::OnlineStateStore> store,
                    ratelimit::ClockFn now);

    GatewayPipeline(const GatewayPipeline&) = delete;
    GatewayPipeline& operator=(const GatewayPipeline&) = delete;

    // 缝①：新连接接入，按对端 IP 限流。返回结果供 install() 决定是否断连。
    ratelimit::RateLimitResult on_accept(const std::string& peer_ip);

    // 缝②：首包/token 鉴权。成功填充 player_id_out 并返回 true（失败=非法 token）。
    bool authenticate(const std::string& token, std::uint64_t& player_id_out);

    // 缝③：选后端。player_id -> game 后端 id（空串 = 无可用后端）。
    std::string route_backend(std::uint64_t player_id) const;

    // 缝④：会话建立/迁移完成 -> 写在线态。
    void on_session_established(std::uint64_t player_id, const std::string& backend);
    // 缝④：会话关闭/迁移走 -> 清在线态。
    void on_session_closed(std::uint64_t player_id);

    // 运行时封禁/放行源 IP（网关运维/风控调用，直通限流层）。
    void ban(const std::string& ip, std::int64_t ttl_ms);
    void allow(const std::string& ip);

    // 在线态存储访问（供上层查询玩家当前落点/迁移控制器定位）。
    std::shared_ptr<redis::OnlineStateStore> get_store() const { return store_; }

    // 一键接线到 ConnectionManager：仅设置既有 on_accept 挂钩（限流 deny 即 close），
    // 不改 ConnectionManager 逻辑本体。auth/route/online 三缝由上层登录/迁移流程
    // 显式调用本对象的对应方法（Connection 不携带 player_id，强行塞入钩子会扭曲设计）。
    void install(connection::ConnectionManager& mgr);

private:
    ratelimit::RateLimiter rl_;
    security::TokenAuth auth_;
    router::Router router_;
    std::shared_ptr<redis::OnlineStateStore> store_;
    ratelimit::ClockFn now_;
};

}  // namespace integration
}  // namespace gateway
}  // namespace cami
