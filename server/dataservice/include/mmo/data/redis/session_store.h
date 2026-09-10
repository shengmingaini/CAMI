// server/dataservice/include/mmo/data/redis/session_store.h
//
// TASK-027 §7 / §15.6 / §15.7：SessionStore 实现 TASK-009 的 gateway::ISessionStore，
// 把网关会话落到缓存（键 sess:{id}，TTL 与 gateway grace 期一致 = 30min），并提供
// sess:player:{player_id} 反查索引（FindByPlayer / 防双开）。
//
// 依赖方向（§27.3）：server/dataservice → server/gateway，**只消费 gateway/session 下的
// 公开头**（ISessionStore 契约 + Session POD），禁止 include gateway 的 src/。
// Redis 不是会话的权威 Owner（§21）：权威在 Gateway 内存，本类只提供「跨进程可重连」的
// 存储能力，供网关侧异步调用，禁止在 Tick 内同步访问。

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "mmo/core/error/result.h"
#include "mmo/data/health.h"
#include "mmo/data/redis/circuit_breaker.h"
#include "mmo/data/redis/connection_pool.h"
#include "mmo/data/redis/redis_config.h"
#include "mmo/gateway/session/session.h"
#include "mmo/gateway/session/session_store.h"

namespace mmo::data::redis {

// ---- 键空间（§8：会话主键 sess:{session_id}；反查索引延伸到同一 sess: 前缀下）----
std::string SessionStoreKey(gateway::SessionId id);           // sess:{id}
std::string SessionPlayerIndexKey(gateway::PlayerId player);  // sess:player:{player_id}

// ---- Session ↔ 缓存值编解码（自带 magic + 布局版本；损坏/截断返回错误而非崩溃）----
std::string EncodeSession(const gateway::Session& s);
core::Result<gateway::Session> DecodeSession(std::string_view blob);

struct SessionStoreOptions {
    // §8：sess TTL 30min，与 gateway 的断线 grace 期一致（单位 ms，避免 duration 隐式转换歧义）。
    core::DurationMs ttl{30 * 60 * 1000};
};

class SessionStore final : public gateway::ISessionStore {
public:
    // 两处重载刻意避开「带 NSDMI 的 struct 作默认实参」在 GCC 下的已知编译失败
    // （TASK-007 EventBus / TASK-009 已踩过同一个坑）。
    static core::Result<std::unique_ptr<SessionStore>> Create(const RedisConfig& cfg);
    static core::Result<std::unique_ptr<SessionStore>> Create(const RedisConfig& cfg,
                                                              const SessionStoreOptions& opt);

    // ---- gateway::ISessionStore（签名与 TASK-009 契约一致，冻结后不可破坏性变更）----
    core::Result<void> Put(const gateway::Session& session) override;
    core::Result<std::optional<gateway::Session>> Get(gateway::SessionId id) override;
    core::Result<std::optional<gateway::Session>> FindByPlayer(gateway::PlayerId player) override;
    core::Result<void> Remove(gateway::SessionId id) override;

    // ---- 运维 / 测试 ----
    HealthStatus Health() const noexcept;
    PoolStats pool_stats() const noexcept;
    const SessionStoreOptions& options() const noexcept { return opt_; }

private:
    SessionStore(const RedisConfig& cfg, const SessionStoreOptions& opt,
                 std::unique_ptr<ConnectionPool> pool);

    core::Result<void> SetRaw(std::string_view key, std::string_view val);
    core::Result<std::optional<std::string>> GetRaw(std::string_view key);
    core::Result<void> DelRaw(std::string_view key);

    RedisConfig                     cfg_;
    SessionStoreOptions             opt_;
    std::unique_ptr<ConnectionPool> pool_;
    mutable CircuitBreaker          breaker_;
};

}  // namespace mmo::data::redis
