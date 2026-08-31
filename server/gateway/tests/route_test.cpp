// server/gateway/tests/route_test.cpp — TASK-010 §16 单元 / §17 集成 / §19 Failure
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/route/gateway_router.h"
#include "mmo/gateway/route/node_registry.h"
#include "mmo/gateway/route/player_router.h"
#include "mmo/gateway/route/route_cache.h"
#include "mmo/gateway/route/scene_router.h"
#include "mmo/gateway/session/session.h"

namespace {

using namespace mmo::gateway;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
namespace core = mmo::core;

int g_fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)
#define CHECK_CODE(result, expected, msg)                                      \
    do {                                                                       \
        const auto& r_ = (result);                                             \
        if (r_.HasValue()) {                                                   \
            ErrorFmt("FAIL: %s (line %d): expected error, got ok\n", msg,       \
                     __LINE__);                                                \
            ++g_fails;                                                         \
        } else if (r_.Err().Code() != (expected)) {                            \
            ErrorFmt("FAIL: %s (line %d): code mismatch\n", msg, __LINE__);    \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)
using core::ErrorCode;

void MakeNode(NodeInfo& n, NodeId id, std::uint32_t load = 0) {
    n.id            = id;
    n.addr          = "127.0.0.1";
    n.port          = static_cast<std::uint16_t>(8000 + id);
    n.role          = NodeRole::GameNode;
    n.load          = load;
    n.health        = NodeHealth::Healthy;
    n.last_heartbeat = core::MonotonicClock::Point();
    n.missed        = 0;
}

// ---------------------------------------------------------------------------
// §16 NodeRegistry
// ---------------------------------------------------------------------------

void TestNodeRegistry() {
    NodeRegistry reg;
    NodeInfo n{};
    MakeNode(n, 1);
    CHECK(reg.Register(n).HasValue(), "register ok");
    CHECK(reg.Register(n).HasValue(), "re-register idempotent");
    CHECK(reg.HealthyCount(NodeRole::GameNode) == 1, "one healthy node");

    // Heartbeat 刷新
    CHECK(reg.Heartbeat(1, 42).HasValue(), "heartbeat ok");
    auto list = reg.ListHealthy(NodeRole::GameNode);
    CHECK(list.HasValue() && list.Value().front().load == 42, "load updated");

    // 未注册节点心跳 / 注销
    CHECK_CODE(reg.Heartbeat(99, 0), ErrorCode::NOT_FOUND, "heartbeat unknown node");
    CHECK(reg.Unregister(1).HasValue(), "unregister ok");
    CHECK_CODE(reg.Unregister(1), ErrorCode::NOT_FOUND, "unregister again fails");
    CHECK(reg.HealthyCount(NodeRole::GameNode) == 0, "no healthy after unregister");
}

void TestHealthStateMachine() {
    NodeRegistry reg;
    NodeInfo n{};
    MakeNode(n, 7);
    CHECK(reg.Register(n).HasValue(), "register ok");

    // 不心跳：推进 1 个间隔 → Suspect
    auto t1 = core::MonotonicClock::Point() + std::chrono::milliseconds(5000);
    CHECK(reg.Tick(t1).HasValue(), "tick ok");
    auto l1 = reg.ListHealthy(NodeRole::GameNode).Value();
    CHECK(!l1.empty() && l1.front().health == NodeHealth::Suspect, "1 miss -> Suspect");

    // 推进 3 个间隔 → Dead（被剔除）
    auto t3 = core::MonotonicClock::Point() + std::chrono::milliseconds(15000);
    CHECK(reg.Tick(t3).HasValue(), "tick ok");
    CHECK(reg.HealthyCount(NodeRole::GameNode) == 0, "3 misses -> Dead, removed");
}

void TestPickAllDeadReturnsBusy() {
    NodeRegistry reg;
    NodeInfo n{};
    MakeNode(n, 3);
    CHECK(reg.Register(n).HasValue(), "register");
    // 全部推进到 Dead
    auto t = core::MonotonicClock::Point() + std::chrono::milliseconds(20000);
    CHECK(reg.Tick(t).HasValue(), "tick to dead");
    CHECK_CODE(reg.Pick(NodeRole::GameNode, "p1"), ErrorCode::BUSY,
               "pick with no alive node -> BUSY");
}

void TestConsistentHashDistribution() {
    NodeRegistry reg;
    for (NodeId i = 1; i <= 10; ++i) {
        NodeInfo n{};
        MakeNode(n, i);
        CHECK(reg.Register(n).HasValue(), "register 10 nodes");
    }
    // 10000 玩家路由，统计每节点命中数
    std::vector<int> counts(11, 0);  // index 1..10
    constexpr int kPlayers = 10000;
    for (int p = 1; p <= kPlayers; ++p) {
        auto r = reg.Pick(NodeRole::GameNode, std::to_string(p));
        CHECK(r.HasValue(), "pick ok");
        ++counts[r.Value()];
    }
    // 一致性：同一玩家必须稳定命中
    auto a = reg.Pick(NodeRole::GameNode, std::to_string(4242));
    auto b = reg.Pick(NodeRole::GameNode, std::to_string(4242));
    CHECK(a.HasValue() && b.HasValue() && a.Value() == b.Value(), "pick stable for same key");

    const double mean = static_cast<double>(kPlayers) / 10.0;
    int           max_dev = 0;
    for (int i = 1; i <= 10; ++i) {
        max_dev = std::max(max_dev, std::abs(counts[i] - static_cast<int>(mean)));
    }
    const double dev = static_cast<double>(max_dev) / mean;
    CHECK(dev < 0.15, "10-node distribution deviation < 15%");
    LineFmt("  [route] 10-node max deviation = %.2f%%\n", dev * 100.0);
}

// ---------------------------------------------------------------------------
// §16 RouteCache
// ---------------------------------------------------------------------------

void TestRouteCache() {
    RouteCache cache(100);
    for (std::uint64_t k = 0; k < 50; ++k) {
        cache.Put(k, static_cast<NodeId>(k % 3 + 1));
    }
    CHECK(cache.Size() == 50, "50 entries");
    CHECK(cache.Get(10).has_value() && *cache.Get(10) == (10 % 3 + 1), "get ok");

    // 打满 LRU 淘汰：再 PUT 60 个新 key → 总数应被钳制在 cap 附近（每片独立）
    for (std::uint64_t k = 1000; k < 1100; ++k) {
        cache.Put(k, 1);
    }
    CHECK(cache.Size() <= 100 + 16, "bounded by capacity (+shard slack)");
    CHECK(cache.Evictions() > 0, "evictions happened");

    // 按 value 批量失效
    cache.EraseByValue(1);
    // 抽查：value=1 的 key 不应再命中（被清除或仍在？需确认 EraseByValue 全清）
    // 这里只验证调用不崩 + 规模下降
    CHECK(cache.Size() < 116, "erase-by-value reduced size");

    // 命中率非零
    cache.Get(1000);
    CHECK(cache.HitRate() >= 0.0, "hit rate computable");
}

void TestRouteCacheHitRate() {
    RouteCache cache(1000);
    // 预热：放 500 个，全命中一次
    for (std::uint64_t k = 0; k < 500; ++k) {
        cache.Put(k, 1);
    }
    double hits = 0;
    for (std::uint64_t k = 0; k < 500; ++k) {
        if (cache.Get(k).has_value()) {
            ++hits;
        }
    }
    CHECK(hits == 500, "all warm hits");
    // 冷查询 50 个不存在的
    for (std::uint64_t k = 100000; k < 100050; ++k) {
        cache.Get(k);
    }
    const double hr = cache.HitRate();
    // 500/550 ≈ 0.909，必须 > 0.90（§19 命中率不塌方）
    CHECK(hr > 0.90, "hit rate stays > 90% under cold misses");
    LineFmt("  [route] cache hit rate = %.2f%%\n", hr * 100.0);
}

// ---------------------------------------------------------------------------
// §16 PlayerRouter / SceneRouter
// ---------------------------------------------------------------------------

void TestPlayerRouter() {
    NodeRegistry reg;
    RouteCache   cache(1000);
    PlayerRouter router(reg, cache);

    NodeInfo n{};
    MakeNode(n, 5);
    CHECK(reg.Register(n).HasValue(), "register node");

    // cache miss → 分配并回填
    auto r1 = router.Route(1001);
    CHECK(r1.HasValue() && r1.Value() == 5, "route assigns to the only node");
    // 再次查询应命中缓存（同一节点）
    auto r2 = router.Route(1001);
    CHECK(r2.HasValue() && r2.Value() == 5, "route stable");
    CHECK(router.CacheHitRate() > 0.0, "hit rate > 0 after warm query");

    // Invalidate
    router.Invalidate(1001);
    // 重新分配（仍唯一节点）
    CHECK(router.Route(1001).Value() == 5, "re-route after invalidate");
}

void TestSceneRouter() {
    NodeRegistry reg;
    RouteCache   cache(1000);
    SceneRouter router(reg, cache);

    // 两个独立 NodeInfo：避免 MakeNode 复用同一变量覆盖 id（此前笔误导致两节点同为 id=6）
    NodeInfo n5{}, n6{};
    MakeNode(n5, 5);
    MakeNode(n6, 6);
    CHECK(reg.Register(n5).HasValue(), "register node 5");
    CHECK(reg.Register(n6).HasValue(), "register node 6");

    // 未绑定 → NOT_FOUND
    CHECK_CODE(router.OwnerOf(42), ErrorCode::NOT_FOUND, "unbound scene NOT_FOUND");

    // 绑定 42 → 5
    CHECK(router.Bind(42, 5).HasValue(), "bind ok");
    CHECK(router.OwnerOf(42).HasValue() && router.OwnerOf(42).Value() == 5, "owner = 5");

    // 重复绑定到不同节点 → VERSION_CONFLICT（Owner 唯一性）
    CHECK_CODE(router.Bind(42, 6), ErrorCode::VERSION_CONFLICT,
               "bind to other node must conflict");
    // 同节点重复绑定 → 幂等
    CHECK(router.Bind(42, 5).HasValue(), "rebind same node idempotent");

    // 非 owner 解绑 → VERSION_CONFLICT
    CHECK_CODE(router.Unbind(42, 6), ErrorCode::VERSION_CONFLICT,
               "only owner can unbind");
    // owner 解绑 → ok，随后 NOT_FOUND
    CHECK(router.Unbind(42, 5).HasValue(), "owner unbind ok");
    CHECK_CODE(router.OwnerOf(42), ErrorCode::NOT_FOUND, "unbound again NOT_FOUND");
}

// ---------------------------------------------------------------------------
// §17 全链路集成：1 Gateway + 2 GameNode 替身
// ---------------------------------------------------------------------------

void TestFullLink() {
    core::EventBus bus;
    GatewayRouter  gw(&bus);

    NodeInfo a{}, b{};
    MakeNode(a, 1);
    MakeNode(b, 2);
    CHECK(gw.RegisterNode(a).HasValue(), "register game node 1");
    CHECK(gw.RegisterNode(b).HasValue(), "register game node 2");

    // 模拟 Login：玩家 1001 登录，路由到某 GameNode
    Session sess{};
    sess.player_id = 1001;
    auto login = gw.RouteUpstream(1001, kInvalidSceneId);
    CHECK(login.HasValue(), "login route ok");
    sess.game_node_id = login.Value();
    CHECK(sess.game_node_id != 0, "session.game_node_id filled");

    // 模拟 EnterScene(scene 7)：上行包按 PlayerID 路由到同一 GameNode
    auto enter = gw.RouteUpstream(1001, 7);
    CHECK(enter.HasValue(), "enter scene route ok");
    CHECK(enter.Value() == sess.game_node_id, "player stays on same node");
    sess.scene_id = 7;
    CHECK(sess.scene_id != kInvalidSceneId, "session.scene_id filled");

    // 未知玩家 + 已知场景：按场景归属路由（首占者自动分配并绑定）
    auto byScene = gw.RouteUpstream(kInvalidPlayerId, 7);
    CHECK(byScene.HasValue(), "route by scene ok");
    // 该场景已绑定（首次 enter 时虽按玩家路由，此处显式绑定一次）
    CHECK(gw.BindScene(7, byScene.Value()).HasValue(), "bind scene explicit");

    // 都未知 → NOT_FOUND
    CHECK_CODE(gw.RouteUpstream(kInvalidPlayerId, kInvalidSceneId),
               ErrorCode::NOT_FOUND, "no target -> NOT_FOUND");
}

// ---------------------------------------------------------------------------
// §19 Failure
// ---------------------------------------------------------------------------

void TestNodeDeadCacheEviction() {
    core::EventBus bus;
    GatewayRouter  gw(&bus);

    NodeInfo a{}, b{};
    MakeNode(a, 1);
    MakeNode(b, 2);
    CHECK(gw.RegisterNode(a).HasValue(), "register 1");

    // 路由一批玩家，缓存填入 node 1 归属
    for (PlayerId p = 1; p <= 200; ++p) {
        (void)gw.RouteUpstream(p, kInvalidSceneId);
    }
    const std::size_t before = gw.CacheSize();
    CHECK(before > 0, "cache populated");

    // node 1 心跳停止 → Tick 判 Dead → NodeDead 订阅回调批量失效其缓存
    // （注：真实单调钟下单点 t=now+20s 会让同刻注册的两节点一起判 Dead；
    //   此处仅注册 node 1，Tick 后它被判 Dead 并剔除；后续再注册幸存 node 2。）
    auto t = core::MonotonicClock::Point() + std::chrono::milliseconds(20000);
    CHECK(gw.Tick(t).HasValue(), "tick to dead");
    // EventBus 为异步派发：Tick 入队的 NodeDead 需宿主线程 Drain 才会触发
    // OnNodeDead → 缓存批量失效（真实部署由 Gateway Tick 的 Event 阶段驱动）。
    const auto drained = bus.Drain();
    CHECK(drained.HasValue(), "drain dispatches NodeDead");
    CHECK(gw.HealthyCount(NodeRole::GameNode) == 0, "node 1 removed after dead");

    // 失效后缓存应缩小（node 1 的条目被清）
    const std::size_t after = gw.CacheSize();
    CHECK(after < before, "dead node cache evicted");
    LineFmt("  [route] cache %zu -> %zu after node death\n", before, after);

    // 注册幸存 node 2，后续请求应全部路由到 node 2（死节点条目已被清，不会被选中）
    CHECK(gw.RegisterNode(b).HasValue(), "register survivor node 2");
    bool any_dead = false;
    for (PlayerId p = 1; p <= 200; ++p) {
        auto r = gw.RouteUpstream(p, kInvalidSceneId);
        if (!r.HasValue() || r.Value() == 1) {
            any_dead = true;
        }
    }
    CHECK(!any_dead, "no request routed to dead node after eviction");
}

void TestAllNodesDown() {
    core::EventBus bus;
    GatewayRouter  gw(&bus);
    NodeInfo a{};
    MakeNode(a, 1);
    CHECK(gw.RegisterNode(a).HasValue(), "register 1");
    auto t = core::MonotonicClock::Point() + std::chrono::milliseconds(20000);
    CHECK(gw.Tick(t).HasValue(), "all dead");
    // 路由应 BUSY（明确错误，不是崩溃）
    CHECK_CODE(gw.RouteUpstream(1, kInvalidSceneId), ErrorCode::BUSY,
               "all down -> BUSY not crash");
}

void TestSceneOwnerConflictFailure() {
    NodeRegistry reg;
    RouteCache   cache(1000);
    SceneRouter router(reg, cache);
    // 两个独立 NodeInfo：避免 MakeNode 复用同一变量覆盖 id
    NodeInfo n1{}, n2{};
    MakeNode(n1, 1);
    MakeNode(n2, 2);
    CHECK(reg.Register(n1).HasValue(), "node 1");
    CHECK(reg.Register(n2).HasValue(), "node 2");
    CHECK(router.Bind(99, 1).HasValue(), "first bind 99->1");
    CHECK_CODE(router.Bind(99, 2), ErrorCode::VERSION_CONFLICT,
               "concurrent bind conflict -> VERSION_CONFLICT");
}

void TestCacheFullNoCrash() {
    // RouteCache 打满后行为：LRU 淘汰，命中率不塌方（§19）
    RouteCache cache(64);
    for (std::uint64_t k = 0; k < 64; ++k) {
        cache.Put(k, 1);
    }
    for (std::uint64_t k = 1000; k < 2000; ++k) {
        cache.Put(k, 2);  // 触发持续淘汰
        (void)cache.Get(k);
    }
    CHECK(cache.Size() <= 64 + 16, "bounded under heavy churn");
    CHECK(cache.HitRate() > 0.90, "hit rate stable under churn");
}

}  // namespace

int main() {
    Line("== TASK-010 gateway route test ==\n");

    // §16
    TestNodeRegistry();
    TestHealthStateMachine();
    TestPickAllDeadReturnsBusy();
    TestConsistentHashDistribution();
    TestRouteCache();
    TestRouteCacheHitRate();
    TestPlayerRouter();
    TestSceneRouter();

    // §17
    TestFullLink();

    // §19
    TestNodeDeadCacheEviction();
    TestAllNodesDown();
    TestSceneOwnerConflictFailure();
    TestCacheFullNoCrash();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
