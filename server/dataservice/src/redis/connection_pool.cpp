// server/dataservice/src/redis/connection_pool.cpp
//
// TASK-027 · 连接池实现（§15.2）。依赖第三方客户端库（hiredis），仅在 src/ 内引用。

#include "mmo/data/redis/connection_pool.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <hiredis/hiredis.h>

#include "mmo/core/error/error_code.h"
#include "mmo/data/redis/retry.h"

namespace mmo::data::redis {

// 编译期护栏（§15.5）：retry.h 的数值镜像必须与 hiredis 真值逐项一致。
// 首版 bug 的根因是「按顺序假设」写魔数（4=TIMEOUT / 3=IO），实际 4=PROTOCOL、3=EOF，
// 使真实超时(6) 落空。这里把该假设固化为编译期断言，hiredis 升级改动编号即构建失败。
static_assert(static_cast<int>(kRedisErrIo) == REDIS_ERR_IO, "retry.h 镜像漂移: Io");
static_assert(static_cast<int>(kRedisErrOther) == REDIS_ERR_OTHER, "retry.h 镜像漂移: Other");
static_assert(static_cast<int>(kRedisErrEof) == REDIS_ERR_EOF, "retry.h 镜像漂移: Eof");
static_assert(static_cast<int>(kRedisErrProtocol) == REDIS_ERR_PROTOCOL,
              "retry.h 镜像漂移: Protocol");
static_assert(static_cast<int>(kRedisErrOom) == REDIS_ERR_OOM, "retry.h 镜像漂移: Oom");
static_assert(static_cast<int>(kRedisErrTimeout) == REDIS_ERR_TIMEOUT,
              "retry.h 镜像漂移: Timeout");

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

    // §19：单次操作超时必须落到 socket 上，否则慢查询（服务端阻塞）会让客户端挂死而非
    // 返回 TIMEOUT。redisSetTimeout 设置 SO_RCVTIMEO/SO_SNDTIMEO；到期后 hiredis 置
    // c->err = REDIS_ERR_TIMEOUT（= 6，**不是** 1..5 的顺序值）且 errstr = "recv timeout"，
    // 由 MapCtxErr 映射为 ErrorCode::TIMEOUT。实测 op_timeout=200ms 时调用耗时 203ms。
    struct timeval op_tv;
    op_tv.tv_sec = static_cast<long>(cfg_.op_timeout.count() / 1000);
    op_tv.tv_usec = static_cast<long>((cfg_.op_timeout.count() % 1000) * 1000);
    if (redisSetTimeout(c, op_tv) != REDIS_OK) {
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
                    // 口径统一（§19）：后端不可用一律 BUSY（core 无 UNAVAILABLE 码），
                    // 与 Create 的探测失败保持同码，便于上层按 IsRetryable 统一退避。
                    return core::Result<PooledConnection>::Fail(core::Error(
                        core::ErrorCode::BUSY, "redis connect failed", core::domain::kData));
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
