// gateway/redis/redis_state.cpp
// 真实 Redis Cluster 后端（在线态 + 16 分片由 Redis Cluster 拓扑承载）。
//
// 门控在 CAMI_BUILD_MODULES=ON（vcpkg 已声明 redis-plus-plus）。
// 轻量 CI（OFF）不编译真实后端；离线可用后端为 InMemoryState（见 in_memory_state.cpp），
// 保证 selfcheck / 单测始终可跑。真实路径在 CI 的 MODULES=ON job 中端到端验证。
#ifdef CAMI_BUILD_MODULES

#include "gateway/redis/online_state.h"

#include <memory>
#include <stdexcept>

#include <sw/redis++/redis_cluster.h>

namespace cami::gateway::redis {

// 真实在线态存储：基于 redis-plus-plus 的 Redis Cluster 客户端。
// - set_online -> HSET online:<pid> backend <game>
// - set_offline -> DEL online:<pid>
// - get_backend -> HGET online:<pid> backend
// - 自动切换：由 Redis Cluster 原生 failover（主从 + 哨兵）提供，客户端自动重定向。
class RedisClusterState : public OnlineStateStore {
public:
    explicit RedisClusterState(const std::string& cluster_uri)
        : rc_(std::make_shared<sw::redis::RedisCluster>(cluster_uri)) {}

    void set_online(uint64_t player_id, const std::string& backend) override {
        rc_->hset(key(player_id), "backend", backend);
    }
    void set_offline(uint64_t player_id) override { rc_->del(key(player_id)); }
    std::optional<std::string> get_backend(uint64_t player_id) const override {
        auto v = rc_->hget(key(player_id), "backend");
        if (v && !v->empty()) return *v;
        return std::nullopt;
    }
    bool is_online(uint64_t player_id) const override { return get_backend(player_id).has_value(); }
    void prune_expired(int64_t) override {
        // 过期由 Redis 侧 TTL / 键空间通知承担；集群规模统计不在此接口。
    }
    std::size_t size() const override { return 0; }

private:
    static std::string key(uint64_t player_id) { return "online:" + std::to_string(player_id); }
    std::shared_ptr<sw::redis::RedisCluster> rc_;
};

std::unique_ptr<OnlineStateStore> make_redis_cluster_state(const std::string& cluster_uri) {
    if (cluster_uri.empty()) return nullptr;
    return std::make_unique<RedisClusterState>(cluster_uri);
}

} // namespace cami::gateway::redis

#endif  // CAMI_BUILD_MODULES
