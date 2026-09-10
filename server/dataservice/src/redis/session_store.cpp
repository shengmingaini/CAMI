// server/dataservice/src/redis/session_store.cpp
//
// TASK-027 §15.6 / §15.7：SessionStore 实现 TASK-009 的 gateway::ISessionStore。
// 值编解码自带 magic + 布局版本；读写统一经「熔断前置 + 指数退避重试」（§15.5 / §15.8）。
// 键：sess:{id}（会话体）+ sess:player:{pid}（反查索引，同 TTL）。

#include "mmo/data/redis/session_store.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <hiredis/hiredis.h>

#include "mmo/data/redis/retry.h"

namespace mmo::data::redis {

namespace {

constexpr char kMagic[4] = {'S', 'E', 'S', '1'};
constexpr std::uint8_t kLayoutVersion = 1;
// 布局：magic(4) + layout(1) + session_id(8) + player_id(8) + last_heartbeat(8)
//       + conn_id(8) + trace_id(8) + gateway_id(4) + game_node_id(4) + scene_id(4)
//       + version(4) + state(1) = 62 字节（小端）。
constexpr std::size_t kSessionBlobSize = 62;

void PutLE32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}
void PutLE64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFull));
}
std::uint32_t GetLE32(std::string_view b, std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + i])) << (8 * i);
    return v;
}
std::uint64_t GetLE64(std::string_view b, std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b[off + i])) << (8 * i);
    return v;
}

core::Error MapCtxErr(redisContext* c) {
    // 与 RedisCache 同口径，使用 hiredis 命名常量（编号非顺序：TIMEOUT=6 / PROTOCOL=4 / EOF=3）。
    core::ErrorCode code = core::ErrorCode::INTERNAL_ERROR;
    if (c->err == REDIS_ERR_TIMEOUT) {
        code = core::ErrorCode::TIMEOUT;
    } else if (c->err == REDIS_ERR_IO || c->err == REDIS_ERR_EOF ||
               c->err == REDIS_ERR_OTHER) {
        code = core::ErrorCode::BUSY;
    }
    const char* msg = (c->errstr[0] != '\0') ? c->errstr : "session store context error";
    return core::Error(code, msg, core::domain::kData);
}

/// 与 RedisCache 同一套语义：熔断前置 + 重试（指数退避）+ 失败计数。
template <typename F>
auto RunGuarded(ConnectionPool& pool, CircuitBreaker& breaker, const RedisConfig& cfg, F&& f)
    -> decltype(f(static_cast<redisContext*>(nullptr))) {
    using R = decltype(f(static_cast<redisContext*>(nullptr)));
    if (!breaker.AllowRequest()) {
        return R::Fail(core::Error(core::ErrorCode::BUSY, "circuit open", core::domain::kData));
    }
    const int attempts = static_cast<int>(cfg.max_retries) + 1;
    core::Error last(core::ErrorCode::TIMEOUT, "session store op failed", core::domain::kData);
    for (int i = 0; i < attempts; ++i) {
        auto conn = pool.Acquire(cfg.op_timeout);
        if (!conn.HasValue()) {
            last = conn.Err();
            if (core::IsRetryable(last.Code())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1 << i));
                continue;
            }
            breaker.RecordFailure();
            return R::Fail(std::move(last));
        }
        auto res = f(conn.Value().handle());
        if (res.HasValue()) {
            breaker.RecordSuccess();
            return res;
        }
        last = res.Err();
        if (core::IsRetryable(last.Code())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1 << i));
            continue;
        }
        breaker.RecordFailure();
        return res;
    }
    breaker.RecordFailure();
    return R::Fail(std::move(last));
}

}  // namespace

std::string SessionStoreKey(gateway::SessionId id) { return "sess:" + std::to_string(id); }

std::string SessionPlayerIndexKey(gateway::PlayerId player) {
    return "sess:player:" + std::to_string(player);
}

std::string EncodeSession(const gateway::Session& s) {
    std::string out;
    out.reserve(kSessionBlobSize);
    out.append(kMagic, 4);
    out.push_back(static_cast<char>(kLayoutVersion));
    PutLE64(out, static_cast<std::uint64_t>(s.session_id));
    PutLE64(out, static_cast<std::uint64_t>(s.player_id));
    PutLE64(out, static_cast<std::uint64_t>(s.last_heartbeat.time_since_epoch().count()));
    PutLE64(out, static_cast<std::uint64_t>(s.conn_id));
    PutLE64(out, static_cast<std::uint64_t>(s.trace_id));
    PutLE32(out, static_cast<std::uint32_t>(s.gateway_id));
    PutLE32(out, static_cast<std::uint32_t>(s.game_node_id));
    PutLE32(out, static_cast<std::uint32_t>(s.scene_id));
    PutLE32(out, s.version);
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(s.state)));
    return out;
}

core::Result<gateway::Session> DecodeSession(std::string_view blob) {
    if (blob.size() < kSessionBlobSize) {
        return core::Result<gateway::Session>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "session blob truncated", core::domain::kData));
    }
    if (std::memcmp(blob.data(), kMagic, 4) != 0) {
        return core::Result<gateway::Session>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "bad session magic", core::domain::kData));
    }
    if (static_cast<std::uint8_t>(blob[4]) != kLayoutVersion) {
        return core::Result<gateway::Session>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "unsupported session layout", core::domain::kData));
    }
    const std::uint8_t st = static_cast<std::uint8_t>(blob[61]);
    if (st > static_cast<std::uint8_t>(gateway::SessionState::Closed)) {
        return core::Result<gateway::Session>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "bad session state", core::domain::kData));
    }
    gateway::Session s;
    s.session_id = static_cast<gateway::SessionId>(GetLE64(blob, 5));
    s.player_id = static_cast<gateway::PlayerId>(GetLE64(blob, 13));
    s.last_heartbeat = core::SteadyTime(
        std::chrono::nanoseconds(static_cast<std::int64_t>(GetLE64(blob, 21))));
    s.conn_id = static_cast<net::ConnectionId>(GetLE64(blob, 29));
    s.trace_id = static_cast<core::TraceID>(GetLE64(blob, 37));
    s.gateway_id = static_cast<gateway::GatewayId>(GetLE32(blob, 45));
    s.game_node_id = static_cast<gateway::NodeId>(GetLE32(blob, 49));
    s.scene_id = static_cast<gateway::SceneId>(GetLE32(blob, 53));
    s.version = GetLE32(blob, 57);
    s.state = static_cast<gateway::SessionState>(st);
    return core::Result<gateway::Session>::Ok(std::move(s));
}

SessionStore::SessionStore(const RedisConfig& cfg, const SessionStoreOptions& opt,
                           std::unique_ptr<ConnectionPool> pool)
    : cfg_(cfg), opt_(opt), pool_(std::move(pool)) {}

core::Result<std::unique_ptr<SessionStore>> SessionStore::Create(const RedisConfig& cfg) {
    return Create(cfg, SessionStoreOptions{});
}

core::Result<std::unique_ptr<SessionStore>> SessionStore::Create(const RedisConfig& cfg,
                                                                const SessionStoreOptions& opt) {
    auto pool = ConnectionPool::Create(cfg);
    if (!pool.HasValue()) {
        return core::Result<std::unique_ptr<SessionStore>>::Fail(pool.Err());
    }
    return core::Result<std::unique_ptr<SessionStore>>::Ok(
        std::unique_ptr<SessionStore>(new SessionStore(cfg, opt, std::move(pool).Value())));
}

core::Result<void> SessionStore::SetRaw(std::string_view key, std::string_view val) {
    const long long ttl_ms = static_cast<long long>(opt_.ttl.count());
    return RunGuarded(*pool_, breaker_, cfg_, [&](redisContext* c) {
        redisReply* r = static_cast<redisReply*>(redisCommand(c, "SET %b %b PX %lld", key.data(),
            key.size(), val.data(), val.size(), ttl_ms));
        if (r == nullptr || c->err != 0) {
            if (r) freeReplyObject(r);
            return core::Result<void>::Fail(MapCtxErr(c));
        }
        freeReplyObject(r);
        return core::Result<void>::Ok();
    });
}

core::Result<std::optional<std::string>> SessionStore::GetRaw(std::string_view key) {
    return RunGuarded(*pool_, breaker_, cfg_, [&](redisContext* c) {
        redisReply* r =
            static_cast<redisReply*>(redisCommand(c, "GET %b", key.data(), key.size()));
        if (r == nullptr || c->err != 0) {
            if (r) freeReplyObject(r);
            return core::Result<std::optional<std::string>>::Fail(MapCtxErr(c));
        }
        if (r->type == REDIS_REPLY_NIL) {
            freeReplyObject(r);
            return core::Result<std::optional<std::string>>::Ok(std::nullopt);
        }
        if (r->type != REDIS_REPLY_STRING) {
            freeReplyObject(r);
            return core::Result<std::optional<std::string>>::Fail(core::Error(
                core::ErrorCode::INTERNAL_ERROR, "unexpected reply", core::domain::kData));
        }
        std::string out(r->str, static_cast<std::size_t>(r->len));
        freeReplyObject(r);
        return core::Result<std::optional<std::string>>::Ok(
            std::optional<std::string>(std::move(out)));
    });
}

core::Result<void> SessionStore::DelRaw(std::string_view key) {
    return RunGuarded(*pool_, breaker_, cfg_, [&](redisContext* c) {
        redisReply* r =
            static_cast<redisReply*>(redisCommand(c, "DEL %b", key.data(), key.size()));
        if (r == nullptr || c->err != 0) {
            if (r) freeReplyObject(r);
            return core::Result<void>::Fail(MapCtxErr(c));
        }
        freeReplyObject(r);
        return core::Result<void>::Ok();
    });
}

core::Result<void> SessionStore::Put(const gateway::Session& session) {
    if (session.session_id == gateway::kInvalidSessionId) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
            "session_id invalid (allocate first)", core::domain::kData));
    }
    auto r = SetRaw(SessionStoreKey(session.session_id), EncodeSession(session));
    if (!r.HasValue()) return r;
    if (session.player_id != gateway::kInvalidPlayerId) {
        auto r2 = SetRaw(SessionPlayerIndexKey(session.player_id),
                         std::to_string(session.session_id));
        if (!r2.HasValue()) return r2;
    }
    return core::Result<void>::Ok();
}

core::Result<std::optional<gateway::Session>> SessionStore::Get(gateway::SessionId id) {
    using R = core::Result<std::optional<gateway::Session>>;
    if (id == gateway::kInvalidSessionId) {
        return R::Ok(std::nullopt);  // 幂等：非法 ID 视作不存在
    }
    auto raw = GetRaw(SessionStoreKey(id));
    if (!raw.HasValue()) return R::Fail(raw.Err());
    if (!raw.Value().has_value()) return R::Ok(std::nullopt);
    auto dec = DecodeSession(*raw.Value());
    if (!dec.HasValue()) return R::Fail(dec.Err());
    return R::Ok(std::optional<gateway::Session>(std::move(dec.Value())));
}

core::Result<std::optional<gateway::Session>> SessionStore::FindByPlayer(gateway::PlayerId player) {
    using R = core::Result<std::optional<gateway::Session>>;
    if (player == gateway::kInvalidPlayerId) {
        return R::Ok(std::nullopt);
    }
    auto idx = GetRaw(SessionPlayerIndexKey(player));
    if (!idx.HasValue()) return R::Fail(idx.Err());
    if (!idx.Value().has_value()) return R::Ok(std::nullopt);
    std::uint64_t id = 0;
    for (char ch : *idx.Value()) {
        if (ch < '0' || ch > '9') {
            return R::Fail(core::Error(core::ErrorCode::INTERNAL_ERROR,
                "corrupt session player index", core::domain::kData));
        }
        id = id * 10ull + static_cast<std::uint64_t>(ch - '0');
    }
    return Get(static_cast<gateway::SessionId>(id));
}

core::Result<void> SessionStore::Remove(gateway::SessionId id) {
    if (id == gateway::kInvalidSessionId) {
        return core::Result<void>::Ok();  // 幂等
    }
    // 先读回会话体以拿到 player_id，再清理反查索引；读失败不阻断主键删除。
    gateway::PlayerId pid = gateway::kInvalidPlayerId;
    auto raw = GetRaw(SessionStoreKey(id));
    if (raw.HasValue() && raw.Value().has_value()) {
        auto dec = DecodeSession(*raw.Value());
        if (dec.HasValue()) pid = dec.Value().player_id;
    }
    auto r = DelRaw(SessionStoreKey(id));
    if (!r.HasValue()) return r;
    if (pid != gateway::kInvalidPlayerId) {
        auto r2 = DelRaw(SessionPlayerIndexKey(pid));
        if (!r2.HasValue()) return r2;
    }
    return core::Result<void>::Ok();
}

HealthStatus SessionStore::Health() const noexcept { return pool_->Health(); }

PoolStats SessionStore::pool_stats() const noexcept { return pool_->Stats(); }

}  // namespace mmo::data::redis
