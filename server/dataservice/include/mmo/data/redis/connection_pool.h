// server/dataservice/include/mmo/data/redis/connection_pool.h
//
// TASK-027 · 连接池（§7 / §15.2）。固定大小池、获取超时、空闲检测、断线重连、指标。

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"
#include "mmo/data/health.h"
#include "mmo/data/redis/redis_config.h"

struct redisContext;  // 前向声明，避免公开头泄漏第三方客户端头

namespace mmo::data::redis {

/// 借出的连接句柄：离开作用域自动归还池中（move-only）。
class PooledConnection {
public:
    PooledConnection() = default;
    explicit PooledConnection(redisContext* ctx, class ConnectionPool* pool)
        : ctx_(ctx), pool_(pool) {}

    ~PooledConnection();

    PooledConnection(PooledConnection&& o) noexcept
        : ctx_(o.ctx_), pool_(o.pool_) {
        o.ctx_ = nullptr;
        o.pool_ = nullptr;
    }
    PooledConnection& operator=(PooledConnection&& o) noexcept {
        if (this != &o) {
            Reset();
            ctx_ = o.ctx_;
            pool_ = o.pool_;
            o.ctx_ = nullptr;
            o.pool_ = nullptr;
        }
        return *this;
    }

    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;

    redisContext* handle() const noexcept { return ctx_; }
    explicit operator bool() const noexcept { return ctx_ != nullptr; }

private:
    void Reset() noexcept;
    redisContext* ctx_{nullptr};
    class ConnectionPool* pool_{nullptr};
};

class ConnectionPool {
public:
    /// 创建并预热连接；无法连通时返回明确错误（不崩溃，允许上层降级）。
    static core::Result<std::unique_ptr<ConnectionPool>> Create(const RedisConfig& cfg);

    ~ConnectionPool();

    /// 获取一个连接：有空闲则复用；否则在容量内新建；池满则等待至超时，
    /// 超时返回 BUSY 并计数（不死锁、不挂死）。
    core::Result<PooledConnection> Acquire(mmo::core::DurationMs timeout);

    PoolStats Stats() const noexcept;
    HealthStatus Health() const noexcept;

    /// 归还连接（由 ~PooledConnection 调用）。
    void Release(redisContext* ctx) noexcept;

private:
    explicit ConnectionPool(const RedisConfig& cfg);
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    redisContext* ConnectOne() const;  // 新建一条连接（含 AUTH / SELECT）

    RedisConfig cfg_;
    mutable std::mutex mtx_;
    std::vector<redisContext*> idle_;
    std::size_t in_use_{0};
    mutable PoolStats stats_;
    bool usable_{false};  // Create 成功预热后置 true
};

}  // namespace mmo::data::redis
