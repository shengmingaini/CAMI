#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "gateway/router/router.h"

using namespace cami::gateway::router;

TEST(Router, EmptyRingNoBackend) {
    Router r(RouterConfig{});
    EXPECT_TRUE(r.route("any").empty());
    EXPECT_EQ(r.backend_count(), 0u);
}

TEST(Router, RoutesAndDeterministic) {
    Router r(RouterConfig{});
    r.add_backend("A");
    r.add_backend("B");
    const std::string a = r.route("player-123");
    EXPECT_FALSE(a.empty());
    EXPECT_EQ(r.route("player-123"), a);
}

TEST(Router, MigrationUnderTenPercent) {
    Router r(RouterConfig{});
    std::vector<std::string> keys;
    for (int i = 0; i < 20000; ++i) keys.push_back("player-" + std::to_string(i));
    std::vector<std::string> base;
    for (int i = 0; i < 20; ++i) base.push_back("node" + std::to_string(i));
    r.reload_backends(base);
    std::vector<std::string> minus1 = base;
    minus1.pop_back();
    EXPECT_LT(r.migration_ratio(keys, minus1), 0.10);  // 移除 1/20 ≈ 5%
    std::vector<std::string> plus1 = base;
    plus1.push_back("node20");
    EXPECT_LT(r.migration_ratio(keys, plus1), 0.10);   // 新增 1/21 ≈ 4.8%
}

TEST(Router, VirtualNodesBalanceLoad) {
    Router r(RouterConfig{});
    std::vector<std::string> keys;
    for (int i = 0; i < 20000; ++i) keys.push_back("player-" + std::to_string(i));
    std::vector<std::string> base;
    for (int i = 0; i < 10; ++i) base.push_back("node" + std::to_string(i));
    r.reload_backends(base);
    std::unordered_map<std::string, int> load;
    for (const auto& k : keys) ++load[r.route(k)];
    const double avg = static_cast<double>(keys.size()) / static_cast<double>(base.size());
    int max_load = 0;
    for (const auto& kv : load) max_load = std::max(max_load, kv.second);
    EXPECT_LE(max_load, 2 * avg);
}

TEST(Router, HotReloadReplacesRing) {
    Router r(RouterConfig{});
    r.reload_backends({"A", "B", "C"});
    bool sawA = false;
    for (int i = 0; i < 20000; ++i) {
        if (r.route("player-" + std::to_string(i)) == "A") { sawA = true; break; }
    }
    r.reload_backends({"X", "Y"});
    bool stillA = false;
    for (int i = 0; i < 20000; ++i) {
        if (r.route("player-" + std::to_string(i)) == "A") { stillA = true; break; }
    }
    EXPECT_TRUE(sawA);
    EXPECT_FALSE(stillA);
}
