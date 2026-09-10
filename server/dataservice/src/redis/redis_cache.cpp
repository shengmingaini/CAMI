// server/dataservice/src/redis/redis_cache.cpp
//
// TASK-027 · RedisCache 实现 TASK-026 的 ICache（§7 / §15.3 / §15.4）。
// 熔断（§15.8）+ 重试（§15.5）统一经 RunGuarded；前缀失效走 SCAN（§21 禁止全量键）。

#include "mmo/data/redis/redis_cache.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <hiredis/hiredis.h>

#include "mmo/data/redis/circuit_breaker.h"
#include "mmo/data/redis/connection_pool.h"
#include "mmo/data/redis/retry.h"
#include "mmo/data/redis/serialization.h"

namespace mmo::data::redis {

namespace {

core::Error MapCtxErr(redisContext* c) {
    core::ErrorCode code = core::ErrorCode::INTERNAL_ERROR;
    if (c->err == 4 /*REDIS_ERR_TIMEOUT*/)
        code = core::ErrorCode::TIMEOUT;
    else if (c->err == 3 /*REDIS_ERR_IO*/)
        code = core::ErrorCode::BUSY;
    const char* msg = (c->errstr[0] != '\0') ? c->errstr : "redis context error";
    return core::Error(code, msg, core::domain::kData);
}

/// 统一执行：熔断前置 + 重试（指数退避）+ 失败计数。
template <typename F>
auto RunGuarded(ConnectionPool& pool, CircuitBreaker& breaker, const RedisConfig& cfg, F&& f)
    -> decltype(f(static_cast<redisContext*>(nullptr))) {
    using R = decltype(f(static_cast<redisContext*>(nullptr)));
    if (!breaker.AllowRequest()) {
        return R::Fail(core::Error(core::ErrorCode::BUSY, "circuit open", core::domain::kData));
    }
    const int attempts = static_cast<int>(cfg.max_retries) + 1;
    core::Error last(core::ErrorCode::TIMEOUT, "redis op failed", core::domain::kData);
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

core::Result<std::unique_ptr<RedisCache>> RedisCache::Create(const RedisConfig& cfg) {
    auto pool = ConnectionPool::Create(cfg);
    if (!pool.HasValue()) {
        return core::Result<std::unique_ptr<RedisCache>>::Fail(pool.Err());
    }
    return core::Result<std::unique_ptr<RedisCache>>::Ok(
        std::unique_ptr<RedisCache>(new RedisCache(std::move(pool).Value(), cfg)));
}

core::Result<std::optional<Record>> RedisCache::Get(const DataKey& key) {
    return RunGuarded(*pool_, breaker_, cfg_, [this, &key](redisContext* c) {
        redisReply* r = static_cast<redisReply*>(
            redisCommand(c, "GET %b", key.data(), static_cast<size_t>(key.size())));
        if (r == nullptr || c->err != 0) {
            if (r) freeReplyObject(r);
            return core::Result<std::optional<Record>>::Fail(MapCtxErr(c));
        }
        if (r->type == REDIS_REPLY_NIL) {
            freeReplyObject(r);
            return core::Result<std::optional<Record>>::Ok(std::nullopt);
        }
        if (r->type != REDIS_REPLY_STRING) {
            freeReplyObject(r);
            return core::Result<std::optional<Record>>::Fail(core::Error(
                core::ErrorCode::INTERNAL_ERROR, "unexpected reply", core::domain::kData));
        }
        auto dec = DecodeRecord(std::string_view(r->str, r->len));
        freeReplyObject(r);
        if (!dec.HasValue()) return core::Result<std::optional<Record>>::Fail(dec.Err());
        return core::Result<std::optional<Record>>::Ok(std::optional<Record>(std::move(dec.Value())));
    });
}

core::Result<void> RedisCache::Put(const Record& rec, mmo::core::DurationMs ttl) {
    const std::string val = EncodeRecord(rec);
    const long long ttl_ms = static_cast<long long>(ttl.count());
    return RunGuarded(*pool_, breaker_, cfg_, [&](redisContext* c) {
        redisReply* r;
        if (ttl_ms > 0) {
            r = static_cast<redisReply*>(redisCommand(c, "SET %b %b PX %lld", rec.key.data(),
                static_cast<size_t>(rec.key.size()), val.data(), static_cast<size_t>(val.size()),
                ttl_ms));
        } else {
            r = static_cast<redisReply*>(redisCommand(c, "SET %b %b", rec.key.data(),
                static_cast<size_t>(rec.key.size()), val.data(), static_cast<size_t>(val.size())));
        }
        if (r == nullptr || c->err != 0) {
            if (r) freeReplyObject(r);
            return core::Result<void>::Fail(MapCtxErr(c));
        }
        freeReplyObject(r);
        return core::Result<void>::Ok();
    });
}

core::Result<void> RedisCache::Invalidate(const DataKey& key) {
    return RunGuarded(*pool_, breaker_, cfg_, [&](redisContext* c) {
        redisReply* r = static_cast<redisReply*>(
            redisCommand(c, "DEL %b", key.data(), static_cast<size_t>(key.size())));
        if (r == nullptr || c->err != 0) {
            if (r) freeReplyObject(r);
            return core::Result<void>::Fail(MapCtxErr(c));
        }
        freeReplyObject(r);
        return core::Result<void>::Ok();
    });
}

core::Result<void> RedisCache::InvalidatePrefix(std::string_view prefix) {
    const std::string pattern = std::string(prefix) + "*";
    return RunGuarded(*pool_, breaker_, cfg_, [&](redisContext* c) {
        std::string cursor = "0";
        do {
            redisReply* r = static_cast<redisReply*>(redisCommand(
                c, "SCAN %s MATCH %b COUNT 100", cursor.c_str(), pattern.data(),
                static_cast<size_t>(pattern.size())));
            if (r == nullptr || c->err != 0) {
                if (r) freeReplyObject(r);
                return core::Result<void>::Fail(MapCtxErr(c));
            }
            if (r->type != REDIS_REPLY_ARRAY || r->elements < 2) {
                freeReplyObject(r);
                return core::Result<void>::Fail(core::Error(core::ErrorCode::INTERNAL_ERROR,
                    "scan reply malformed", core::domain::kData));
            }
            const char* cur = r->element[0]->str;
            cursor.assign(cur ? cur : "0", cur ? r->element[0]->len : 1);
            redisReply* keys = r->element[1];
            if (keys->elements > 0) {
                for (size_t i = 0; i < keys->elements; ++i) {
                    redisReply* d = static_cast<redisReply*>(redisCommand(
                        c, "DEL %b", keys->element[i]->str,
                        static_cast<size_t>(keys->element[i]->len)));
                    if (d == nullptr || c->err != 0) {
                        if (d) freeReplyObject(d);
                        freeReplyObject(r);
                        return core::Result<void>::Fail(MapCtxErr(c));
                    }
                    freeReplyObject(d);
                }
            }
            freeReplyObject(r);
        } while (cursor != "0");
        return core::Result<void>::Ok();
    });
}

HealthStatus RedisCache::Health() const noexcept { return pool_->Health(); }
PoolStats RedisCache::pool_stats() const noexcept { return pool_->Stats(); }

}  // namespace mmo::data::redis
