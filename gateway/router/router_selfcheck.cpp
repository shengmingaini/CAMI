// gateway/router/router_selfcheck.cpp
// 确定性 selfcheck：固定 key 集合 + FNV-1a，验证路由正确性、迁移率、均衡、热更新。
// 无 GTest、无真实 socket、零外部依赖 —— CAMI_BUILD_MODULES=OFF/ON 均编译通过。
#include "gateway/router/router_selfcheck.h"
#include "gateway/router/router.h"

#include <cstdio>
#include <unordered_map>
#include <vector>

namespace cami::gateway::router {

bool router_selfcheck() {
    bool ok = true;

    // 固定 key 集合（模拟玩家 session key / player_id）
    std::vector<std::string> keys;
    for (int i = 0; i < 20000; ++i) keys.push_back("player-" + std::to_string(i));

    // --- 空环：route 必须返回空（kNoBackend）---
    {
        Router r(RouterConfig{});
        if (!r.route("any").empty()) {
            std::fprintf(stderr, "[FAIL] router: empty ring should route to no backend\n");
            ok = false;
        } else {
            std::printf("[ OK ] router: empty ring routes to nobody\n");
        }
    }

    // --- 基础路由：命中、确定性 ---
    {
        Router r(RouterConfig{});
        r.add_backend("nodeA");
        r.add_backend("nodeB");
        const std::string a = r.route("player-123");
        if (a.empty()) {
            std::fprintf(stderr, "[FAIL] router: route should hit a backend\n");
            ok = false;
        } else if (r.route("player-123") != a) {
            std::fprintf(stderr, "[FAIL] router: route must be deterministic\n");
            ok = false;
        } else {
            std::printf("[ OK ] router: routes and is deterministic (player-123 -> %s)\n",
                        a.c_str());
        }
    }

    // --- 节点增减迁移 < 10% ---
    {
        Router r(RouterConfig{});
        std::vector<std::string> base;
        for (int i = 0; i < 20; ++i) base.push_back("node" + std::to_string(i));  // N=20
        r.reload_backends(base);

        // 移除 1 个节点 -> 迁移率应 ~1/20 ≈ 5% < 10%（一致性哈希 1/N 性质）
        std::vector<std::string> minus1 = base;
        minus1.pop_back();
        const double rm = r.migration_ratio(keys, minus1);
        if (rm >= 0.10) {
            std::fprintf(stderr, "[FAIL] router: remove 1/20 nodes moved %.2f%% (>=10%%)\n",
                         rm * 100.0);
            ok = false;
        } else {
            std::printf("[ OK ] router: remove 1/20 nodes migrates %.2f%% (<10%%)\n",
                        rm * 100.0);
        }

        // 增加 1 个节点 -> 迁移率应 ~1/21 ≈ 4.8% < 10%
        std::vector<std::string> plus1 = base;
        plus1.push_back("node20");
        const double add = r.migration_ratio(keys, plus1);
        if (add >= 0.10) {
            std::fprintf(stderr, "[FAIL] router: add 1 node moved %.2f%% (>=10%%)\n",
                         add * 100.0);
            ok = false;
        } else {
            std::printf("[ OK ] router: add 1 node migrates %.2f%% (<10%%)\n",
                        add * 100.0);
        }
    }

    // --- 虚拟节点均衡：每节点负载接近均值（max < 2x avg）---
    {
        Router r(RouterConfig{});
        std::vector<std::string> base;
        for (int i = 0; i < 10; ++i) base.push_back("node" + std::to_string(i));
        r.reload_backends(base);
        std::unordered_map<std::string, int> load;
        for (const auto& k : keys) ++load[r.route(k)];
        const double avg = static_cast<double>(keys.size()) / static_cast<double>(base.size());
        int max_load = 0;
        for (const auto& kv : load) max_load = std::max(max_load, kv.second);
        if (max_load > 2 * avg) {
            std::fprintf(stderr, "[FAIL] router: load imbalance max=%d avg=%.0f (max>2x)\n",
                         max_load, avg);
            ok = false;
        } else {
            std::printf("[ OK ] router: load balanced (max=%d avg=%.0f)\n", max_load, avg);
        }
    }

    // --- 热更新：reload_backends 原子替换环，旧节点不再命中 ---
    {
        Router r(RouterConfig{});
        r.reload_backends({"nodeA", "nodeB", "nodeC"});
        bool saw_old = false;
        for (const auto& k : keys) {
            if (r.route(k) == "nodeA") { saw_old = true; break; }
        }
        r.reload_backends({"nodeX", "nodeY"});  // 热更新
        bool still_old = false;
        for (const auto& k : keys) {
            if (r.route(k) == "nodeA") { still_old = true; break; }
        }
        if (!saw_old) {
            std::fprintf(stderr, "[FAIL] router: pre-reload should use nodeA\n");
            ok = false;
        } else if (still_old) {
            std::fprintf(stderr, "[FAIL] router: reload did not replace ring (nodeA still hit)\n");
            ok = false;
        } else {
            std::printf("[ OK ] router: hot-reload replaces ring (nodeA evicted)\n");
        }
    }

    return ok;
}

} // namespace cami::gateway::router
