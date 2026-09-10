// server/dataservice/include/mmo/data/redis/redis_cache.h
//
// TASK-027 · RedisCache —— 实现 TASK-026 冻结的 ICache（§7）。
// 行为必须与内存实现（InMemoryCache）一致，可被同一套接口测试覆盖。

#pragma once

#include <memory>

#include "mmo/core/error/result.h"
#include "mmo/data/health.h"
#include "mmo/data/icache.h"
#include "mmo/data/record.h"
#include "mmo/data/redis/circuit_breaker.h"
#include "mmo/data/redis/connection_pool.h"
#include "mmo/data/redis/redis_config.h"

namespace mmo::data::redis {

class RedisCache final : public mmo::data::ICache {
public:
    /// 经连接池创建缓存；无法连通 Redis 时返回明确错误（不崩溃）。
    static core::Result<std::unique_ptr<RedisCache>> Create(const RedisConfig& cfg);

    // ---- ICache ----
    core::Result<std::optional<Record>> Get(const DataKey& key) override;
    core::Result<void> Put(const Record& rec, mmo::core::DurationMs ttl = {}) override;
    core::Result<void> Invalidate(const DataKey& key) override;
    /// 前缀失效：使用 SCAN 分批收集 + 批量 DEL（禁止一次性拉全量键）。
    core::Result<void> InvalidatePrefix(std::string_view prefix) override;

    HealthStatus Health() const noexcept;
    PoolStats pool_stats() const noexcept;

private:
    explicit RedisCache(std::unique_ptr<ConnectionPool> pool, const RedisConfig& cfg)
        : pool_(std::move(pool)), cfg_(cfg) {}

    std::unique_ptr<ConnectionPool> pool_;
    RedisConfig cfg_;
    mutable CircuitBreaker breaker_;
};

}  // namespace mmo::data::redis
