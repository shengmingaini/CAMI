// server/gateway/tests/route_bench.cpp — TASK-010 §18 Benchmark
//
// 指标（机器可读 key=value，供验收脚本 assert_metric 解析）：
//   route_lookup_ns=         单次路由查询耗时（ns），§22 < 100ns
//   cache_hit_rate=          缓存命中率（0..1），§22 > 0.99
//   cache_evict_ns=          单次 LRU 淘汰耗时（ns）
//   registry_tick_us_1k_nodes= 1000 节点 Tick 扫描耗时（us），§22 < 100us
//
// 用法：route_bench [--lookups N]   默认 1000000
// 输出：bench/gateway_route.txt

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/route/gateway_router.h"
#include "mmo/gateway/route/node_registry.h"
#include "mmo/gateway/session/session.h"

namespace {

using namespace mmo::gateway;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
namespace core = mmo::core;

void MakeNode(NodeInfo& n, NodeId id) {
    n.id            = id;
    n.addr          = "127.0.0.1";
    n.port          = static_cast<std::uint16_t>(8000 + id);
    n.role          = NodeRole::GameNode;
    n.load          = id % 100;
    n.health        = NodeHealth::Healthy;
    n.last_heartbeat = core::MonotonicClock::Point();
    n.missed        = 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t lookups = 1'000'000;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--lookups") {
            lookups = static_cast<std::size_t>(std::stoull(argv[i + 1]));
        }
    }

    const std::size_t kNodes = 10;
    const std::size_t kWarm  = 90'000;  // < 默认缓存 100k，保证全命中

    core::EventBus bus;
    GatewayRouter  gw(&bus);
    for (NodeId i = 1; i <= kNodes; ++i) {
        NodeInfo n{};
        MakeNode(n, i);
        if (!gw.RegisterNode(n).HasValue()) {
            ErrorFmt("route_bench: register node %u failed\n", i);
            return 1;
        }
    }

    // 预热：路由 kWarm 个玩家，全部填入缓存
    std::vector<PlayerId> warm;
    warm.reserve(kWarm);
    for (std::size_t i = 0; i < kWarm; ++i) {
        const PlayerId p = static_cast<PlayerId>(i + 1);
        if (!gw.RouteUpstream(p, kInvalidSceneId).HasValue()) {
            ErrorFmt("route_bench: warmup route failed at %zu\n", i);
            return 1;
        }
        warm.push_back(p);
    }
    // 进入测量窗口前重置命中计数：HitRate() 为累计值，warmup 的首次 miss 会
    // 拉低它；重置后测得的是稳态命中率（§22 要求 > 99%）。
    gw.cache().ResetStats();

    // ---- 1. 路由查询耗时（§22 < 100ns）----
    const auto t0 = core::MonotonicClock::Now();
    for (std::size_t i = 0; i < lookups; ++i) {
        (void)gw.RouteUpstream(warm[i % kWarm], kInvalidSceneId);
    }
    const double route_lookup_ns =
        static_cast<double>(core::MonotonicClock::Now() - t0) /
        static_cast<double>(lookups);

    const double cache_hit_rate = gw.CacheHitRate();

    // ---- 2. 单次 LRU 淘汰耗时 ----
    RouteCache small(100);
    for (std::uint64_t k = 0; k < 100; ++k) {
        small.Put(k, 1);
    }
    const auto e0 = core::MonotonicClock::Now();
    small.Put(100'000, 2);  // 触发一次淘汰
    const double cache_evict_ns =
        static_cast<double>(core::MonotonicClock::Now() - e0);

    // ---- 3. 1000 节点 Tick 扫描（§22 < 100us）----
    NodeRegistry reg;
    for (NodeId i = 1; i <= 1000; ++i) {
        NodeInfo n{};
        MakeNode(n, i);
        (void)reg.Register(n);
    }
    const auto tk0 = core::MonotonicClock::Now();
    (void)reg.Tick(core::MonotonicClock::Point());
    const double registry_tick_us_1k_nodes =
        static_cast<double>(core::MonotonicClock::Now() - tk0) / 1000.0;

    // ---- 输出 ----
    Line("== TASK-010 route_bench ==\n");
    LineFmt("lookups=%zu nodes=%zu warm=%zu\n", lookups, kNodes, kWarm);
    LineFmt("route_lookup_ns=%.3f\n", route_lookup_ns);
    LineFmt("cache_hit_rate=%.6f\n", cache_hit_rate);
    LineFmt("cache_evict_ns=%.3f\n", cache_evict_ns);
    LineFmt("registry_tick_us_1k_nodes=%.3f\n", registry_tick_us_1k_nodes);

    std::ofstream ofs("bench/gateway_route.txt");
    if (!ofs) {
        ErrorFmt("route_bench: cannot write bench/gateway_route.txt\n");
        return 1;
    }
    ofs << "lookups=" << lookups << "\n";
    ofs << "nodes=" << kNodes << "\n";
    ofs << "route_lookup_ns=" << route_lookup_ns << "\n";
    ofs << "cache_hit_rate=" << cache_hit_rate << "\n";
    ofs << "cache_evict_ns=" << cache_evict_ns << "\n";
    ofs << "registry_tick_us_1k_nodes=" << registry_tick_us_1k_nodes << "\n";
    ofs.close();

    LineFmt("route_bench done -> bench/gateway_route.txt\n");
    return 0;
}
