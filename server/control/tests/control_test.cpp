// server/control/tests/control_test.cpp — TASK-040 §16 单元 / §17 集成 / §19 Failure
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
// 测试进程内完成：节点注册 / 心跳 / 超时 / 配置版本比对 / 路由失效协调（接 TASK-010
// GatewayRouter）/ Gateway 多实例 / 崩溃降级 / 可选持久化。

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "test_print.h"
#include "mmo/control/control_service.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/time/clock.h"
#include "mmo/data/idata_store.h"
#include "mmo/gateway/route/gateway_router.h"

namespace {

using namespace mmo::control;
using namespace mmo::gateway;
namespace core = mmo::core;
using core::ErrorCode;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
using mmo::core::test::ErrorFmt;

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

// ---------------------------------------------------------------------------
// §16 节点注册 / 心跳 / 超时 / 拓扑
// ---------------------------------------------------------------------------

void TestNodeLifecycle() {
    core::EventBus bus;
    ControlService cs(bus);

    int reg = 0, hb = 0, off = 0, cfg = 0;
    (void)bus.Subscribe<NodeRegistered>([&](const NodeRegistered&) { ++reg; });
    (void)bus.Subscribe<NodeHeartbeat>([&](const NodeHeartbeat&) { ++hb; });
    (void)bus.Subscribe<NodeOffline>([&](const NodeOffline&) { ++off; });
    (void)bus.Subscribe<ConfigPushed>([&](const ConfigPushed&) { ++cfg; });

    // node1（game）注册 + 心跳 → 作为超时检测对象
    CHECK(cs.RegisterNode(1, ControlRole::GameNode, "10.0.0.1").HasValue(), "register game");
    CHECK(cs.OnlineCount(ControlRole::GameNode) == 1, "one game online");
    CHECK(!cs.FindNode(99).has_value(), "unknown node absent");

    CHECK(cs.Heartbeat(1, 10, 100, 5).HasValue(), "heartbeat");
    auto n1 = cs.FindNode(1);
    CHECK(n1.has_value() && n1->load == 10 && n1->player_count == 100, "load/players updated");

    // 心跳超时 → 判离线（future 超过 heartbeat_timeout=15000ms）
    const auto future = core::MonotonicClock::Point() + std::chrono::milliseconds(20000);
    CHECK(cs.Tick(future).HasValue(), "tick");
    CHECK(cs.OnlineCount(ControlRole::GameNode) == 0, "game offline after timeout");
    auto off1 = cs.FindNode(1);
    CHECK(off1.has_value() && off1->status == NodeStatus::Offline, "status offline");

    // 心跳恢复
    CHECK(cs.Heartbeat(1, 1, 1, 1).HasValue(), "heartbeat recovers");
    CHECK(cs.OnlineCount(ControlRole::GameNode) == 1, "recovered online");

    // node2（gateway）在超时检测之后注册，仅用于幂等 + 优雅下线计数
    CHECK(cs.RegisterNode(2, ControlRole::Gateway, "10.0.0.2").HasValue(), "register gw");
    CHECK(cs.RegisterNode(2, ControlRole::Gateway, "10.0.0.2:9").HasValue(), "re-register idempotent");
    CHECK(cs.OnlineCount(ControlRole::Gateway) == 1, "one gw online");

    // 主动注销（优雅下线）→ 第二条 NodeOffline
    CHECK(cs.UnregisterNode(2).HasValue(), "unregister gw");
    CHECK(!cs.FindNode(2).has_value(), "gw removed");

    (void)bus.Drain();
    CHECK(reg == 3, "3 NodeRegistered (incl re-register)");
    CHECK(hb == 2, "2 NodeHeartbeat");
    CHECK(off == 2, "2 NodeOffline (timeout + unregister)");
    CHECK(cfg == 0, "no ConfigPushed yet");
}

// ---------------------------------------------------------------------------
// §16 配置版本比对 + 原子热加载（复用 TASK-003 原子替换，无锁）
// ---------------------------------------------------------------------------

void TestConfigVersionAndHotLoad() {
    core::EventBus bus;
    ControlService cs(bus);
    CHECK(cs.ConfigVersion() == 0, "initial version 0");
    CHECK(cs.PushConfig(1, "snapshot-v1").HasValue(), "push v1");
    CHECK(cs.ConfigVersion() == 1, "version now 1");

    // 在途读者持有 v1 的 shared_ptr 副本
    auto reader_v1 = cs.CurrentConfig();
    CHECK(reader_v1->version == 1 && reader_v1->snapshot == "snapshot-v1", "reader sees v1");

    // 拒绝旧版本（必须单调）
    CHECK_CODE(cs.PushConfig(1, "stale"), ErrorCode::INVALID_ARGUMENT, "stale rejected");
    // 回退需以更大版本号重推
    CHECK(cs.PushConfig(2, "snapshot-v2").HasValue(), "push v2 (re-pin old with bigger ver)");

    // 旧读者不受影响：原子替换不中断在途读者
    CHECK(reader_v1->version == 1 && reader_v1->snapshot == "snapshot-v1", "old reader unaffected");
    auto reader_v2 = cs.CurrentConfig();
    CHECK(reader_v2->version == 2 && reader_v2->snapshot == "snapshot-v2", "new reader sees v2");
}

// ---------------------------------------------------------------------------
// §15.6 Gateway 多实例：capacity/load 供前置 LB 分流
// ---------------------------------------------------------------------------

void TestGatewayMultiInstance() {
    core::EventBus bus;
    ControlService cs(bus);
    CHECK(cs.RegisterNode(10, ControlRole::Gateway, "gw-a", 100).HasValue(), "gw a");
    CHECK(cs.RegisterNode(11, ControlRole::Gateway, "gw-b", 200).HasValue(), "gw b");
    CHECK(cs.RegisterNode(1, ControlRole::GameNode, "game-1", 0).HasValue(), "game not gw");

    auto gws = cs.GatewayInstances();
    CHECK(gws.size() == 2, "two gateway instances");
    std::uint32_t total_cap = 0;
    for (const auto& g : gws) total_cap += g.capacity;
    CHECK(total_cap == 300, "capacity sum 300");

    CHECK(cs.Heartbeat(10, 40, 400, 8).HasValue(), "gw a reports load");
    auto gwa = cs.FindNode(10);
    CHECK(gwa->load == 40 && gwa->capacity == 100, "gw a load/capacity");
}

// ---------------------------------------------------------------------------
// §17 集成：节点上下线 → 复用 TASK-010 NodeDead → 路由缓存失效
// ---------------------------------------------------------------------------

void TestRouteInvalidationCoordination() {
    core::EventBus bus;
    GatewayRouter gw(&bus);            // 订阅 NodeDead → 失效路由缓存
    ControlService cs(bus);            // 权威节点表，离线时发布 NodeDead

    NodeInfo gi{};
    gi.id = 1;
    gi.addr = "127.0.0.1";
    gi.port = 8001;
    gi.role = NodeRole::GameNode;
    gi.load = 0;
    gi.last_heartbeat = core::MonotonicClock::Point();
    gi.health = NodeHealth::Healthy;
    gi.missed = 0;
    CHECK(gw.RegisterNode(gi).HasValue(), "gw register node 1");

    CHECK(cs.RegisterNode(1, ControlRole::GameNode, "127.0.0.1").HasValue(), "cs register node 1");

    // 路由缓存已分配 key→node1
    gw.cache().Put(1001, 1);
    auto cached = gw.cache().Get(1001);
    CHECK(cached.has_value() && cached.value() == 1, "route cached to node 1");

    // 心跳超时：ControlService 判离线 → 发布 NodeDead
    const auto future = core::MonotonicClock::Point() + std::chrono::milliseconds(20000);
    CHECK(cs.Tick(future).HasValue(), "cs tick offline");
    auto drained = bus.Drain();
    CHECK(drained.HasValue(), "drain dispatches NodeDead");

    // Gateway 的路由缓存应已批量失效
    CHECK(!gw.cache().Get(1001).has_value(), "route cache invalidated after node offline");
    auto info = cs.FindNode(1);
    CHECK(info.has_value() && info->status == NodeStatus::Offline, "cs topology marks offline");
}

// ---------------------------------------------------------------------------
// §19 Failure：ControlService 崩溃 → 游戏节点用本地缓存继续服务，不雪崩
// ---------------------------------------------------------------------------

void TestCrashDegradation() {
    core::EventBus bus;
    // unique_ptr 模拟可销毁的控制面（崩溃 = 对象消失）
    auto cs = std::make_unique<ControlService>(bus);
    CHECK(cs->RegisterNode(1, ControlRole::GameNode, "10.0.0.1").HasValue(), "register");
    CHECK(cs->PushConfig(1, "cfg-v1").HasValue(), "push config");

    // 节点侧本地缓存（拓扑快照 + 配置快照），不依赖控制面存活
    auto cached_topo = cs->QueryTopology();
    auto cached_cfg = cs->CurrentConfig();  // shared_ptr<const>，控制面销毁后副本仍有效
    CHECK(cached_topo.size() == 1, "cached topology has node");
    CHECK(cached_cfg->version == 1, "cached config version 1");

    cs.reset();  // 模拟 ControlService 崩溃

    // 节点凭本地缓存继续服务，不雪崩
    CHECK(cached_topo.size() == 1, "topology snapshot still usable after crash");
    CHECK(cached_cfg->version == 1 && cached_cfg->snapshot == "cfg-v1", "config snapshot still usable");
    CHECK(cached_cfg != nullptr, "shared_ptr keeps payload alive (no avalanche)");
}

// ---------------------------------------------------------------------------
// §13 持久化（可选，经 TASK-026 IDataStore 接口）—— 进程重启恢复
// ---------------------------------------------------------------------------

struct FakeStore : mmo::data::IDataStore {
    std::map<mmo::data::DataKey, mmo::data::Record> map;

    core::Result<std::optional<mmo::data::Record>> Load(const mmo::data::DataKey& key) override {
        auto it = map.find(key);
        if (it == map.end()) {
            return core::Result<std::optional<mmo::data::Record>>::Ok(std::nullopt);
        }
        return core::Result<std::optional<mmo::data::Record>>::Ok(it->second);
    }

    core::Result<void> Save(const mmo::data::Record& rec, mmo::data::VersionCheck vc) override {
        if (vc.required) {
            auto it = map.find(rec.key);
            const std::uint32_t actual = (it == map.end()) ? 0u : it->second.version;
            if (actual != vc.expected_version) {
                return core::Result<void>::Fail(
                    core::Error(ErrorCode::VERSION_CONFLICT, "conflict", "control"));
            }
        }
        mmo::data::Record copy = rec;
        const auto it = map.find(rec.key);
        copy.version = (it == map.end() ? 0u : it->second.version) + 1u;
        map[rec.key] = std::move(copy);
        return core::Result<void>::Ok();
    }

    core::Result<void> Delete(const mmo::data::DataKey& key, mmo::data::VersionCheck) override {
        map.erase(key);
        return core::Result<void>::Ok();
    }

    core::Result<std::vector<mmo::data::Record>> BatchLoad(
        std::span<const mmo::data::DataKey> keys) override {
        std::vector<mmo::data::Record> out;
        for (const auto& k : keys) {
            auto it = map.find(k);
            if (it != map.end()) out.push_back(it->second);
        }
        return core::Result<std::vector<mmo::data::Record>>::Ok(std::move(out));
    }

    core::Result<std::vector<mmo::data::BatchOutcome>> BatchSave(
        std::span<const mmo::data::Record> recs) override {
        std::vector<mmo::data::BatchOutcome> out;
        for (const auto& r : recs) {
            (void)Save(r, mmo::data::VersionCheck{});
            out.push_back(mmo::data::BatchOutcome{r.key, true, 0});
        }
        return core::Result<std::vector<mmo::data::BatchOutcome>>::Ok(std::move(out));
    }
};

void TestPersistence() {
    FakeStore store;
    {
        core::EventBus bus;
        ControlService cs(bus, &store);
        CHECK(cs.RegisterNode(1, ControlRole::GameNode, "10.0.0.1").HasValue(), "register persist");
        CHECK(cs.RegisterNode(2, ControlRole::Gateway, "10.0.0.2", 500).HasValue(), "register gw persist");
        CHECK(cs.PushConfig(3, "cfg-persisted").HasValue(), "push config persist");
    }  // cs 析构，store 保留节点表 + 配置
    {
        core::EventBus bus2;
        ControlService cs2(bus2, &store);  // 模拟重启恢复
        CHECK(cs2.FindNode(1).has_value(), "node1 restored from store");
        auto gw = cs2.FindNode(2);
        CHECK(gw.has_value() && gw->capacity == 500, "gw restored with capacity");
        CHECK(cs2.ConfigVersion() == 3, "config version restored");
        auto cfg = cs2.CurrentConfig();
        CHECK(cfg->snapshot == "cfg-persisted", "config snapshot restored");
    }
}

}  // namespace

int main() {
    Line("== TASK-040 control test ==\n");

    TestNodeLifecycle();
    TestConfigVersionAndHotLoad();
    TestGatewayMultiInstance();
    TestRouteInvalidationCoordination();
    TestCrashDegradation();
    TestPersistence();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
