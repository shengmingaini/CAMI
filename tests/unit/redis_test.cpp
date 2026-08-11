#include <gtest/gtest.h>

#include <vector>

#include "gateway/redis/in_memory_state.h"
#include "gateway/redis/online_state.h"

using namespace cami::gateway::redis;

struct FakeClock {
    int64_t now_ms = 0;
};

TEST(RedisInMemory, SetGetOfflineTtl) {
    FakeClock clk;
    ClockFn now = [&clk]() -> int64_t { return clk.now_ms; };
    InMemoryState st(now, /*ttl_ms=*/1000);
    st.set_online(1001, "game-3");
    EXPECT_EQ(st.get_backend(1001), "game-3");
    EXPECT_TRUE(st.is_online(1001));
    st.set_offline(1001);
    EXPECT_FALSE(st.is_online(1001));
    st.set_online(2002, "game-7");
    clk.now_ms += 1500;  // 超过 TTL
    EXPECT_FALSE(st.is_online(2002));
    st.prune_expired(clk.now_ms);
    EXPECT_EQ(st.size(), 0u);
}

TEST(RedisShard, BalancedAcross16) {
    ShardRouter r;
    std::vector<int> cnt(ShardRouter::kShards, 0);
    for (uint64_t pid = 0; pid < 20000; ++pid) ++cnt[r.shard_of(pid)];
    int maxc = 0;
    for (int c : cnt) maxc = std::max(maxc, c);
    int avg = 20000 / ShardRouter::kShards;
    EXPECT_LE(maxc, 2 * avg);
}

TEST(RedisShard, FailoverReroutes) {
    ShardRouter r;
    int on_zero = 0;
    for (uint64_t pid = 0; pid < 20000; ++pid) {
        if (r.shard_of(pid) == 0) ++on_zero;
    }
    EXPECT_GT(on_zero, 0);  // 分片0确实承载了部分 key
    r.set_shard_down(0, true);
    int still_zero = 0;
    std::vector<int> cnt(ShardRouter::kShards, 0);
    for (uint64_t pid = 0; pid < 20000; ++pid) {
        int s = r.shard_of(pid);
        if (s == 0) ++still_zero;
        if (s >= 0) ++cnt[s];
    }
    EXPECT_EQ(still_zero, 0);  // 全部分流走
    int maxc = 0;
    for (int c : cnt) maxc = std::max(maxc, c);
    EXPECT_LE(maxc, 2 * (20000 / ShardRouter::kShards));
}

#ifdef CAMI_BUILD_MODULES
// 真实后端端到端验证：连 Redis Cluster 做 HSET/HGET/DEL。
// CI 由 MODULES=ON job 提供 grokzen/redis-cluster 服务容器并设置 CAMI_REDIS_CLUSTER_URI。
// 本地 MODULES=ON 无集群时跳过（仍可编过、链上 redis-plus-plus）。
TEST(RedisCluster, RealBackendRoundTrip) {
    const char* uri = std::getenv("CAMI_REDIS_CLUSTER_URI");
    if (!uri || uri[0] == '\0') {
        GTEST_SKIP() << "CAMI_REDIS_CLUSTER_URI not set; skipping live Redis Cluster test";
    }
    auto store = make_redis_cluster_state(uri);
    ASSERT_NE(store, nullptr);

    constexpr uint64_t pid = 99009900;
    store->set_online(pid, "game-real");
    auto backend = store->get_backend(pid);
    ASSERT_TRUE(backend.has_value());
    EXPECT_EQ(*backend, "game-real");
    EXPECT_TRUE(store->is_online(pid));

    store->set_offline(pid);
    EXPECT_FALSE(store->is_online(pid));
}
#endif
