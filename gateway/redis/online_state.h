#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cami::gateway::redis {

// 单调毫秒时钟；selfcheck/单测注入假时钟，生产默认走 steady_clock。
using ClockFn = std::function<int64_t()>;

// 在线态存储抽象：玩家(player_id) -> 所在后端 game 实例。
// 真实后端 = Redis Cluster（16 分片，原生 failover）；原型/轻量 CI = InMemoryState。
class OnlineStateStore {
public:
    virtual ~OnlineStateStore() = default;
    virtual void set_online(uint64_t player_id, const std::string& backend) = 0;
    virtual void set_offline(uint64_t player_id) = 0;
    virtual std::optional<std::string> get_backend(uint64_t player_id) const = 0;
    virtual bool is_online(uint64_t player_id) const = 0;
    virtual void prune_expired(int64_t now_ms) = 0;  // 清理过期条目（TTL）
    virtual std::size_t size() const = 0;
};

// 16 分片路由（一致性哈希 + 故障探测重路由）。
// player_id -> 分片索引 [0, kShards)。分片均衡是验收项；某分片下线时线性探测到下一可用分片（自动切换）。
class ShardRouter {
public:
    static constexpr int kShards = 16;
    explicit ShardRouter(int shards = kShards);
    int shard_of(uint64_t player_id) const;
    int shard_count() const { return shards_; }
    void set_shard_down(int shard, bool down);
    bool is_shard_down(int shard) const;

private:
    static uint64_t hash_fn(uint64_t v);
    int shards_;
    std::vector<char> down_;  // 长度 shards_，非 0 = 下线
};

#ifdef CAMI_BUILD_MODULES
// 真实后端工厂：根据 Redis Cluster URI（如 "redis://host:7000"）构造集群后端。
// 仅在 MODULES=ON 可用（依赖 redis-plus-plus）；轻量 CI / 原型用 InMemoryState。
// 返回 nullptr 仅当 URI 为空（调用方应保证非空）。
std::unique_ptr<OnlineStateStore> make_redis_cluster_state(const std::string& cluster_uri);
#endif

} // namespace cami::gateway::redis
