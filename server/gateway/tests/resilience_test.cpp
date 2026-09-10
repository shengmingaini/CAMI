// server/gateway/tests/resilience_test.cpp — TASK-037 §16 单元 / §17 集成（37.1 HealthMonitor）
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf（红线）。
// 本文件不 include 任何 gateway 内部 src/。

#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/route/node_registry.h"
#include "mmo/gateway/resilience/health_monitor.h"

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

/// 断言 Result 失败且错误码为预期值。
#define CHECK_CODE(result, expected, msg)                                      \
    do {                                                                       \
        const auto& r_ = (result);                                             \
        if (r_.HasValue()) {                                                   \
            ErrorFmt("FAIL: %s (line %d): expected error, got ok\n", msg,       \
                     __LINE__);                                                \
            ++g_fails;                                                         \
        } else if (r_.Err().Code() != (expected)) {                            \
            ErrorFmt("FAIL: %s (line %d): code mismatch\n", msg, __LINE__);     \
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

// ---------------------------------------------------------------------------
// §16 单元：注册 + 健康状态 + 未知节点判定
// ---------------------------------------------------------------------------

void TestRegisterAndStatus() {
    core::EventBus bus;
    HealthMonitor mon(&bus);
    CHECK(mon.Register(MakeNode(1, NodeRole::GameNode, 10)).HasValue(), "register g1 ok");
    CHECK(mon.Register(MakeNode(2, NodeRole::GameNode, 5)).HasValue(), "register g2 ok");
    CHECK(mon.Register(MakeNode(3, NodeRole::SceneNode, 0)).HasValue(), "register s1 ok");
    CHECK(mon.Size() == 3, "size should be 3");
    CHECK(mon.StatusOf(1) == NodeStatus::Healthy, "g1 healthy");
    CHECK(mon.StatusOf(2) == NodeStatus::Healthy, "g2 healthy");
    CHECK(mon.StatusOf(999) == NodeStatus::Dead, "unknown node is Dead");
    CHECK(mon.DeadCount() == 0, "no dead yet");
}

// ---------------------------------------------------------------------------
// §16 单元：心跳恢复（Suspect -> Healthy）
// ---------------------------------------------------------------------------

void TestHeartbeatRecovery() {
    core::EventBus bus;
    HealthMonitor mon(&bus);
    CHECK(mon.Register(MakeNode(1, NodeRole::GameNode, 10)).HasValue(), "register ok");

    // 1 个心跳间隔丢失 -> Suspect
    CHECK(mon.Tick(NowPlus(std::chrono::milliseconds(5000))).HasValue(), "tick +5s ok");
    CHECK(mon.StatusOf(1) == NodeStatus::Suspect, "after 1 missed -> Suspect");

    // 心跳恢复
    CHECK(mon.Heartbeat(1, 12).HasValue(), "heartbeat ok");
    CHECK(mon.StatusOf(1) == NodeStatus::Healthy, "heartbeat recovers to Healthy");

    // 恢复后不到一个间隔再 Tick -> 仍是 Healthy
    CHECK(mon.Tick(NowPlus(std::chrono::milliseconds(4000))).HasValue(), "tick +4s ok");
    CHECK(mon.StatusOf(1) == NodeStatus::Healthy, "still Healthy within one interval");
}

// ---------------------------------------------------------------------------
// §16 单元：3 次丢失 -> Dead，并从目录剔除
// ---------------------------------------------------------------------------

void TestDeadDetectionAndUnregister() {
    core::EventBus bus;
    HealthMonitor mon(&bus);
    CHECK(mon.Register(MakeNode(1, NodeRole::GameNode, 10)).HasValue(), "register ok");

    // 3 个心跳间隔丢失 -> Dead
    CHECK(mon.Tick(NowPlus(std::chrono::milliseconds(15000))).HasValue(), "tick +15s ok");
    CHECK(mon.StatusOf(1) == NodeStatus::Dead, "after 3 missed -> Dead");
    CHECK(mon.DeadCount() == 1, "dead count = 1");

    // 目录里已无该节点：FindReplacement 不再返回它
    auto repl = mon.FindReplacement(NodeRole::GameNode);
    CHECK(repl.HasValue() && repl.Value().empty(), "no live replacement for dead node");
}

// ---------------------------------------------------------------------------
// §16 单元：NodeDead 仅发布一次（不重复）
// ---------------------------------------------------------------------------

void TestNodeDeadPublishedOnce() {
    core::EventBus bus;
    HealthMonitor mon(&bus);
    int dead_count = 0;
    (void)bus.Subscribe<NodeDead>([&](const NodeDead&) { ++dead_count; });

    CHECK(mon.Register(MakeNode(1, NodeRole::GameNode, 10)).HasValue(), "register ok");
    CHECK(mon.Tick(NowPlus(std::chrono::milliseconds(15000))).HasValue(), "tick -> Dead");
    CHECK(mon.Tick(NowPlus(std::chrono::milliseconds(30000))).HasValue(), "tick again (already dead)");
    (void)bus.Drain();
    CHECK(dead_count == 1, "NodeDead must be published exactly once per node");
}

// ---------------------------------------------------------------------------
// §16 单元：替换节点选择按负载升序
// ---------------------------------------------------------------------------

void TestFindReplacementLoadOrder() {
    core::EventBus bus;
    HealthMonitor mon(&bus);
    CHECK(mon.Register(MakeNode(1, NodeRole::GameNode, 30)).HasValue(), "g1 load30");
    CHECK(mon.Register(MakeNode(2, NodeRole::GameNode, 10)).HasValue(), "g2 load10");
    CHECK(mon.Register(MakeNode(3, NodeRole::GameNode, 20)).HasValue(), "g3 load20");

    auto repl = mon.FindReplacement(NodeRole::GameNode);
    CHECK(repl.HasValue(), "find replacement ok");
    const auto& v = repl.Value();
    CHECK(v.size() == 3, "three candidates");
    // 负载升序：2(10) < 3(20) < 1(30)
    CHECK(v.size() == 3 && v[0] == 2 && v[1] == 3 && v[2] == 1, "load-ascending order");

    // 场景角色不混入游戏候选
    auto sc = mon.FindReplacement(NodeRole::SceneNode);
    CHECK(sc.HasValue() && sc.Value().empty(), "no scene node -> empty");
}

// ---------------------------------------------------------------------------
// §16 单元：Dead 节点被替换选择排除
// ---------------------------------------------------------------------------

void TestFindReplacementExcludesDead() {
    core::EventBus bus;
    HealthMonitor mon(&bus);
    CHECK(mon.Register(MakeNode(1, NodeRole::GameNode, 10)).HasValue(), "g1 load10");
    CHECK(mon.Tick(NowPlus(std::chrono::milliseconds(15000))).HasValue(), "g1 -> Dead");
    // 之后注册 g2（全新健康），确保替换集合里只剩它
    CHECK(mon.Register(MakeNode(2, NodeRole::GameNode, 20)).HasValue(), "g2 fresh");

    auto repl = mon.FindReplacement(NodeRole::GameNode);
    CHECK(repl.HasValue() && repl.Value().size() == 1 && repl.Value()[0] == 2,
          "dead node excluded, only g2 remains");
}

// ---------------------------------------------------------------------------
// §16 单元：本地无候选 + affinity -> 一致性哈希 Pick 兜底返回空（不报错）
// ---------------------------------------------------------------------------

void TestFindReplacementFallbackEmpty() {
    core::EventBus bus;
    HealthMonitor mon(&bus);
    auto repl = mon.FindReplacement(NodeRole::GameNode, "player-123");
    CHECK(repl.HasValue() && repl.Value().empty(),
          "fallback with no candidates returns empty, not error");
}

// ---------------------------------------------------------------------------
// §16 单元：注销幂等
// ---------------------------------------------------------------------------

void TestUnregisterIdempotent() {
    core::EventBus bus;
    HealthMonitor mon(&bus);
    CHECK(mon.Register(MakeNode(1, NodeRole::GameNode, 10)).HasValue(), "register ok");
    CHECK(mon.Unregister(1).HasValue(), "unregister ok");
    CHECK(mon.Unregister(1).HasValue(), "unregister again idempotent");
    CHECK(mon.Size() == 0, "size 0 after unregister");
    CHECK(mon.StatusOf(1) == NodeStatus::Dead, "unregistered node is Dead");
}

}  // namespace

int main() {
    Line("== TASK-037 gateway resilience (37.1 HealthMonitor) test ==\n");

    TestRegisterAndStatus();
    TestHeartbeatRecovery();
    TestDeadDetectionAndUnregister();
    TestNodeDeadPublishedOnce();
    TestFindReplacementLoadOrder();
    TestFindReplacementExcludesDead();
    TestFindReplacementFallbackEmpty();
    TestUnregisterIdempotent();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
