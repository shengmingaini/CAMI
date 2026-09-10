// server/dataservice/src/redis/connection_pool.cpp
//
// TASK-027 · 连接池实现（§15.2）。依赖第三方客户端库（hiredis），仅在 src/ 内引用。

#include "mmo/data/redis/connection_pool.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <hiredis/hiredis.h>

#include "mmo/core/error/error_code.h"

namespace mmo::data::redis {

core::Result<std::string> ResolveRedisPassword(const RedisConfig& cfg) {
    if (cfg.password_env.empty()) {
        return core::Result<std::string>::Ok(std::string{});
    }
    const char* p = std::getenv(cfg.password_env.c_str());
    if (p == nullptr) {
        return core::Result<std::string>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "redis password env not set", core::domain::kData));
    }
    return core::Result<std::string>::Ok(std::string(p));
}

ConnectionPool::ConnectionPool(const RedisConfig& cfg) : cfg_(cfg) {}

ConnectionPool::~ConnectionPool() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto* c : idle_) redisFree(c);
    idle_.clear();
}

redisContext* ConnectionPool::ConnectOne() const {
    struct timeval tv;
    tv.tv_sec = static_cast<long>(cfg_.connect_timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((cfg_.connect_timeout.count() % 1000) * 1000);

    redisContext* c = redisConnectWithTimeout(cfg_.host.c_str(), static_cast<int>(cfg_.port), tv);
    if (c == nullptr) return nullptr;
    if (c->err != 0) {
        redisFree(c);
        return nullptr;
    }

    auto pw = ResolveRedisPassword(cfg_);
    if (pw.HasValue() && !pw.Value().empty()) {
        redisReply* r = static_cast<redisReply*>(redisCommand(c, "AUTH %s", pw.Value().c_str()));
        if (r == nullptr || c->err != 0) {
            if (r) freeReplyObject(r);
            redisFree(c);
            return nullptr;
        }
        const bool ok = (r->type == REDIS_REPLY_STATUS);
        freeReplyObject(r);
        if (!ok) {
            redisFree(c);
            return nullptr;
        }
    }

    if (cfg_.database != 0) {
        redisReply* r = static_cast<redisReply*>(redisCommand(c, "SELECT %u", cfg_.database));
        if (r == nullptr || c->err != 0) {
            if (r) freeReplyObject(r);
            redisFree(c);
            return nullptr;
        }
        freeReplyObject(r);
    }
    return c;
}

core::Result<std::unique_ptr<ConnectionPool>> ConnectionPool::Create(const RedisConfig& cfg) {
    auto pw = ResolveRedisPassword(cfg);
    if (!pw.HasValue()) {
        return core::Result<std::unique_ptr<ConnectionPool>>::Fail(pw.Err());
    }
    std::unique_ptr<ConnectionPool> pool(new ConnectionPool(cfg));
    // 探测一次连通性：不通则明确失败（允许上层降级启动，§19）
    redisContext* probe = pool->ConnectOne();
    if (probe == nullptr) {
        return core::Result<std::unique_ptr<ConnectionPool>>::Fail(
            core::Error(core::ErrorCode::BUSY, "cannot connect redis", core::domain::kData));
    }
    redisFree(probe);
    pool->usable_ = true;
    return core::Result<std::unique_ptr<ConnectionPool>>::Ok(std::move(pool));
}

core::Result<PooledConnection> ConnectionPool::Acquire(mmo::core::DurationMs timeout) {
    if (!usable_) {
        return core::Result<PooledConnection>::Fail(
            core::Error(core::ErrorCode::BUSY, "redis pool not usable", core::domain::kData));
    }
    const std::int64_t deadline =
        mmo::core::MonotonicClock::Now() + static_cast<std::int64_t>(timeout.count()) * 1'000'000LL;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!idle_.empty()) {
                redisContext* c = idle_.back();
                idle_.pop_back();
                ++in_use_;
                return core::Result<PooledConnection>::Ok(PooledConnection(c, this));
            }
            if (in_use_ < cfg_.pool_size) {
                redisContext* c = ConnectOne();
                if (c == nullptr) {
                    return core::Result<PooledConnection>::Fail(core::Error(
                        core::ErrorCode::TIMEOUT, "redis connect failed", core::domain::kData));
                }
                ++in_use_;
                return core::Result<PooledConnection>::Ok(PooledConnection(c, this));
            }
            ++stats_.wait_count;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (mmo::core::MonotonicClock::Now() >= deadline) {
            std::lock_guard<std::mutex> lk(mtx_);
            ++stats_.acquire_timeout_count;
            return core::Result<PooledConnection>::Fail(
                core::Error(core::ErrorCode::BUSY, "redis pool exhausted", core::domain::kData));
        }
    }
}

void ConnectionPool::Release(redisContext* ctx) noexcept {
    if (ctx == nullptr) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (ctx->err != 0) {
        redisFree(ctx);  // 断线连接直接丢弃，下次 Acquire 重建
    } else {
        idle_.push_back(ctx);
    }
    if (in_use_ > 0) --in_use_;
}

PoolStats ConnectionPool::Stats() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    PoolStats s = stats_;
    s.idle = idle_.size();
    s.in_use = in_use_;
    return s;
}

HealthStatus ConnectionPool::Health() const noexcept {
    if (!usable_) return HealthStatus::Unavailable;
    std::lock_guard<std::mutex> lk(mtx_);
    if (idle_.empty() && in_use_ >= cfg_.pool_size) return HealthStatus::Degraded;
    return HealthStatus::Healthy;
}

// ---- PooledConnection ----
PooledConnection::~PooledConnection() { Reset(); }

void PooledConnection::Reset() noexcept {
    if (pool_ && ctx_) pool_->Release(ctx_);
    ctx_ = nullptr;
    pool_ = nullptr;
}

}  // namespace mmo::data::redis
