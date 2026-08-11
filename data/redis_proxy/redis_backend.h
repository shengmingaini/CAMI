#pragma once
// ============================================================================
// data/redis_proxy/redis_backend.h — [PRODUCTION] Redis 缓存后端 (Week4 D3 → D6)
// ----------------------------------------------------------------------------
// 实现 CacheBackend 抽象 (接口见 cache_proxy.h):
//   Get/Put(TTL)/Delete/Contains/Size
// 仅在 CAMI_BUILD_MODULES=ON (vcpkg: redis-plus-plus) 下编译, OFF 构建看不到本文件。
//
// 策略:
//   - Put 带默认 TTL, 防止冷数据常驻 (cache-proxy.md §4 红线)。
//   - 缓存双删由 CacheProxy::Delete 负责 (同时清缓存 + 回源删 DB), 本类只管 Redis 侧。
//   - 批量读走 Redis MGET; 批量写走 Pipeline (见 redis_backend.cpp)。
// ============================================================================
#ifdef CAMI_BUILD_MODULES

#include "data/redis_proxy/cache_proxy.h"

#include <chrono>
#include <memory>
#include <string>

#include <sw/redis++/redis.h>

namespace cami {
namespace data {
namespace redis_proxy {

class RedisBackend : public CacheBackend {
public:
    // uri 形如 "redis://127.0.0.1:6379" 或 "redis_cluster://127.0.0.1:7000"。
    // default_ttl 为写入默认过期时间, 防冷数据常驻。
    explicit RedisBackend(const std::string& uri,
                          std::chrono::seconds default_ttl = std::chrono::seconds(300));
    ~RedisBackend() override = default;

    std::optional<std::string> Get(std::string_view key) override;
    void Put(std::string_view key, std::string value) override;
    void Delete(std::string_view key) override;
    bool Contains(std::string_view key) const override;
    std::size_t Size() const override;

    // 批量读: 一次 MGET 取多个 key (降低 RTT)
    std::vector<std::optional<std::string>> MGet(const std::vector<std::string>& keys);
    // 批量写: 走 Pipeline, 带默认 TTL
    void MPut(const std::vector<std::pair<std::string, std::string>>& kvs);

private:
    std::shared_ptr<sw::redis::Redis> rc_;
    std::chrono::seconds ttl_;
};

}  // namespace redis_proxy
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
