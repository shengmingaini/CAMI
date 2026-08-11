// gateway/redis/redis_selfcheck.cpp
// 确定性 selfcheck：内存后端语义（注入假时钟）+ 16 分片均衡 + 故障切换重路由。
// 无 GTest、无真实 socket / Redis、零外部依赖 —— CAMI_BUILD_MODULES=OFF/ON 均编译通过。
#include "gateway/redis/redis_selfcheck.h"
#include "gateway/redis/in_memory_state.h"
#include "gateway/redis/online_state.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace cami::gateway::redis {

bool redis_selfcheck() {
    bool ok = true;

    // 假时钟：内存后端 TTL / prune 走确定性时间。
    struct FakeClock {
        int64_t now_ms = 0;
    };
    FakeClock clk;
    ClockFn now = [&clk]() -> int64_t { return clk.now_ms; };

    // --- InMemoryState 语义 ---
    {
        InMemoryState st(now, /*ttl_ms=*/1000);
        st.set_online(1001, "game-3");
        if (st.get_backend(1001) != "game-3" || !st.is_online(1001)) {
            std::fprintf(stderr, "[FAIL] redis: set/get/is_online broken\n");
            ok = false;
        } else {
            std::printf("[ OK ] redis: set_online/get_backend/is_online round-trip\n");
        }
        st.set_offline(1001);
        if (st.is_online(1001)) {
            std::fprintf(stderr, "[FAIL] redis: set_offline should drop player\n");
            ok = false;
        } else {
            std::printf("[ OK ] redis: set_offline removes player\n");
        }
        // TTL 过期
        st.set_online(2002, "game-7");
        clk.now_ms += 1500;  // 超过 1000ms TTL
        if (st.is_online(2002)) {
            std::fprintf(stderr, "[FAIL] redis: TTL expiry should mark offline\n");
            ok = false;
        } else {
            std::printf("[ OK ] redis: TTL expiry marks offline\n");
        }
        // prune 清理过期条目
        st.prune_expired(clk.now_ms);
        if (st.size() != 0) {
            std::fprintf(stderr, "[FAIL] redis: prune_expired should clear expired\n");
            ok = false;
        } else {
            std::printf("[ OK ] redis: prune_expired clears expired entries\n");
        }
    }

    // --- 16 分片均衡 ---
    {
        ShardRouter r;
        std::vector<int> cnt(ShardRouter::kShards, 0);
        for (uint64_t pid = 0; pid < 20000; ++pid) ++cnt[r.shard_of(pid)];
        const int maxc = *std::max_element(cnt.begin(), cnt.end());
        const int avg = static_cast<int>(20000 / ShardRouter::kShards);
        if (maxc > 2 * avg) {
            std::fprintf(stderr, "[FAIL] redis: shard imbalance max=%d avg=%d (>2x)\n", maxc, avg);
            ok = false;
        } else {
            std::printf("[ OK ] redis: 16-shard balanced (max=%d avg=%d)\n", maxc, avg);
        }
    }

    // --- 故障切换：下线分片 0，原落其上的 key 全部重路由，分布仍均衡 ---
    {
        ShardRouter r;
        int on_zero = 0;
        for (uint64_t pid = 0; pid < 20000; ++pid) {
            if (r.shard_of(pid) == 0) ++on_zero;
        }
        r.set_shard_down(0, true);
        int still_zero = 0;
        std::vector<int> cnt(ShardRouter::kShards, 0);
        for (uint64_t pid = 0; pid < 20000; ++pid) {
            const int s = r.shard_of(pid);
            if (s == 0) ++still_zero;
            if (s >= 0) ++cnt[s];
        }
        const int maxc = *std::max_element(cnt.begin(), cnt.end());
        const int avg = static_cast<int>(20000 / ShardRouter::kShards);
        if (still_zero != 0) {
            std::fprintf(stderr, "[FAIL] redis: down shard 0 still routed (%d keys)\n", still_zero);
            ok = false;
        } else if (maxc > 2 * avg) {
            std::fprintf(stderr, "[FAIL] redis: post-failover imbalance max=%d (>2x avg)\n", maxc);
            ok = false;
        } else {
            std::printf("[ OK ] redis: failover reroutes %d keys, balanced (max=%d)\n", on_zero,
                        maxc);
        }
    }

    return ok;
}

} // namespace cami::gateway::redis
