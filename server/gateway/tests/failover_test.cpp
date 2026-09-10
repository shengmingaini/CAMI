// server/gateway/tests/failover_test.cpp — TASK-037 §16 单元 / §17 集成（37.3 FailoverCoordinator）
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf（红线）。

#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/route/node_registry.h"
#include "mmo/gateway/resilience/health_monitor.h"
#include "mmo/gateway/resilience/failover_coordinator.h"

namespace {

using namespace mmo::gateway;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;

namespace core = mmo::core;
using core::ErrorCode;

int g_fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

NodeInfo MakeNode(NodeId id, NodeRole role, std::uint32_t load) {
    NodeInfo n{};
    n.id   = id;
    n.addr = "127.0.0.1";
    n.port = static_cast<std::uint16_t>(8000 + id);
    n.role = role;
    n.load = load;
    return n;
}

core::SteadyTime NowPlus(std::chrono::milliseconds delta) noexcept {
    return core::MonotonicClock::Point() + delta;
}

// 先注册 A 并 Tick 杀死 A（B/C 尚未注册，不会被误杀），随后再注册替换节点。
// 注意：所有节点注册时间相同；若一起注册再 Tick，B/C 也会因心跳超时被判 Dead。
void KillNodeA(HealthMonitor& health, NodeId a = 1) {
    (void)health.Register(MakeNode(a, NodeRole::GameNode, 100));
    (void)health.Tick(NowPlus(std::chrono::milliseconds(15000)));
}

// ---- 注入接口 stub ----

struct StubDirectory : ISessionDirectory {
    std::vector<SessionId> on_a;  // 位于「故障节点 A(=1)」的会话
    std::vector<SessionId> SessionsOnNode(NodeId node) const noexcept override {
        if (node == 1) return on_a;
        return {};
    }
};

struct StubRebinder : ISessionRebinder {
    std::vector<std::pair<SessionId, NodeId>> calls;
    std::size_t max_in_flight{0};
    std::size_t fail_for{0};  // 对该 SessionId 的 Rebind 返回失败
    const FailoverCoordinator* coord{nullptr};
    core::Result<void> Rebind(SessionId id, NodeId to) noexcept override {
        calls.push_back({id, to});
        if (coord != nullptr) {
            max_in_flight = std::max(max_in_flight, coord->InFlight());
        }
        if (id == fail_for) {
            return core::Result<void>::Fail(
                core::Error{ErrorCode::BUSY, "rebinder busy", core::domain::kNet});
        }
        return core::Result<void>::Ok();
    }
};

// ---------------------------------------------------------------------------
// §16 单元：OnNodeDead 选同角色低负载替换节点并迁移
// ---------------------------------------------------------------------------

void TestOnNodeDeadSelectsLowestLoad() {
    core::EventBus bus;
    HealthMonitor health(&bus);
    KillNodeA(health);  // A 已 Dead
    CHECK(health.StatusOf(1) == NodeStatus::Dead, "A is Dead");
    CHECK(health.Register(MakeNode(2, NodeRole::GameNode, 10)).HasValue(), "register B load10");
    CHECK(health.Register(MakeNode(3, NodeRole::GameNode, 20)).HasValue(), "register C load20");

    StubDirectory dir;
    dir.on_a = {101, 102};
    StubRebinder reb;
    FailoverCoordinator coord(health, dir, reb, /*max_concurrent=*/64);
    reb.coord = &coord;

    CHECK(coord.OnNodeDead(1, /*trace=*/1).HasValue(), "OnNodeDead ok");
    CHECK(coord.Stats().detected == 1, "detected = 1");
    CHECK(coord.Stats().reattached == 2, "both sessions reattached");
    // 替换节点应为负载最低的 B（id=2），而非 C
    CHECK(!reb.calls.empty() && reb.calls[0].second == 2, "rebound to lowest-load B");
    CHECK(coord.QueueDepth() == 0, "queue drained");
    CHECK(coord.InFlight() == 0, "in_flight back to 0");
}

// ---------------------------------------------------------------------------
// §16 单元：批量限速（1000 会话，max_concurrent=64，不雪崩）
// ---------------------------------------------------------------------------

void TestRateLimit() {
    core::EventBus bus;
    HealthMonitor health(&bus);
    KillNodeA(health);
    CHECK(health.Register(MakeNode(2, NodeRole::GameNode, 0)).HasValue(), "register B");

    StubDirectory dir;
    dir.on_a.reserve(1000);
    for (std::size_t i = 0; i < 1000; ++i) {
        dir.on_a.push_back(static_cast<SessionId>(1000 + i));
    }
    StubRebinder reb;
    constexpr std::size_t kMax = 64;
    FailoverCoordinator coord(health, dir, reb, kMax);
    reb.coord = &coord;

    CHECK(coord.OnNodeDead(1, 1).HasValue(), "OnNodeDead ok");
    CHECK(coord.Stats().reattached == 1000, "all 1000 reattached");
    CHECK(reb.max_in_flight <= kMax, "concurrency never exceeded max_concurrent");
    CHECK(reb.max_in_flight == kMax, "fully packed waves (== max_concurrent)");
    CHECK(coord.QueueDepth() == 0, "queue drained");
}

// ---------------------------------------------------------------------------
// §19 Failure：无可用替换节点（不崩溃，记入 detected，reattached=0）
// ---------------------------------------------------------------------------

void TestNoReplacement() {
    core::EventBus bus;
    HealthMonitor health(&bus);
    KillNodeA(health);  // 仅 A，无替换节点

    StubDirectory dir;
    dir.on_a = {101, 102};
    StubRebinder reb;
    FailoverCoordinator coord(health, dir, reb, 64);
    reb.coord = &coord;

    CHECK(coord.OnNodeDead(1, 1).HasValue(), "OnNodeDead must not crash");
    CHECK(coord.Stats().detected == 1, "detected = 1");
    CHECK(coord.Stats().reattached == 0, "no reattach without replacement");
    CHECK(reb.calls.empty(), "no rebind attempted");
}

// ---------------------------------------------------------------------------
// §19 Failure：部分会话重绑定失败 -> 计入 failed，不中断其余
// ---------------------------------------------------------------------------

void TestPartialRebindFailure() {
    core::EventBus bus;
    HealthMonitor health(&bus);
    KillNodeA(health);
    CHECK(health.Register(MakeNode(2, NodeRole::GameNode, 0)).HasValue(), "register B");

    StubDirectory dir;
    dir.on_a = {101, 102, 103};
    StubRebinder reb;
    reb.fail_for = 102;  // 中间一个会话失败
    FailoverCoordinator coord(health, dir, reb, 64);
    reb.coord = &coord;

    CHECK(coord.OnNodeDead(1, 1).HasValue(), "OnNodeDead ok");
    CHECK(coord.Stats().reattached == 2, "two succeeded");
    CHECK(coord.Stats().failed == 1, "one failed");
    CHECK(coord.Stats().reattached + coord.Stats().failed == 3, "all accounted for");
}

}  // namespace

int main() {
    Line("== TASK-037 gateway resilience (37.3 FailoverCoordinator) test ==\n");
    TestOnNodeDeadSelectsLowestLoad();
    TestRateLimit();
    TestNoReplacement();
    TestPartialRebindFailure();
    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
