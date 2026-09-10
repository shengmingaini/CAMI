// benchmark/resilience/resilience_bench.cpp — TASK-037 §18 Benchmark
//
// 跨模块容灾基准：覆盖 37.1/37.2/37.3（Gateway 侧）与 37.4（Scene 恢复）。
// 指标（机器可读 key=value，供验收脚本 assert_metric 解析）：
//   detect_ms=                    Dead 节点检测耗时（单次 HealthMonitor::Tick 扫描）
//   reattach_ms_per_player=       单玩家故障接管迁移耗时（OnNodeDead 总耗时 / 玩家数）
//   scene_restore_ms=             Scene 从最近 Checkpoint 重建耗时
//   checkpoint_ms=                Checkpoint 采集耗时
//   checkpoint_bytes=             SceneCheckpoint 结构序列化尺寸（对账/带宽估算）
//
// 用法：resilience_bench [--players N]   默认 players=1000
// 输出：bench/resilience.txt（同时打印 stdout）

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/route/node_registry.h"
#include "mmo/gateway/session/session.h"
#include "mmo/gateway/resilience/health_monitor.h"
#include "mmo/gateway/resilience/failover_coordinator.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/recovery/scene_recovery.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/scene/scene_manager.h"

namespace {

namespace core = mmo::core;
namespace gtw  = mmo::gateway;
namespace game = mmo::game;
namespace rec  = mmo::game::scene::recovery;

using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;

// ---- 37.3 注入接口 stub（与上游 DONE 模块解耦，§27.3）----
class FakeSessionDir final : public gtw::ISessionDirectory {
public:
    FakeSessionDir(gtw::NodeId dead, std::size_t n) : dead_(dead) {
        ids_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) ids_.push_back(1000 + i);
    }
    std::vector<gtw::SessionId> SessionsOnNode(gtw::NodeId node) const noexcept override {
        return (node == dead_) ? ids_ : std::vector<gtw::SessionId>{};
    }
    std::size_t Count() const noexcept { return ids_.size(); }
private:
    gtw::NodeId               dead_;
    std::vector<gtw::SessionId> ids_;
};

class FakeRebinder final : public gtw::ISessionRebinder {
public:
    core::Result<void> Rebind(gtw::SessionId, gtw::NodeId) noexcept override {
        ++count_;
        return core::Result<void>::Ok();
    }
    std::size_t count_{0};
};

// ---- 37.4 工厂适配：把 SceneManager 暴露给 SceneRecovery ----
class SceneManagerFactory final : public rec::ISceneFactory {
public:
    explicit SceneManagerFactory(game::SceneManager& m) : mgr_(m) {}
    core::Result<game::Scene*> Create(game::SceneId id, game::SceneType t,
                                     game::NodeId owner) override {
        return mgr_.Create(id, t, owner);
    }
private:
    game::SceneManager& mgr_;
};

void Run(std::size_t players) {
    // =====================================================================
    // 37.1 + 37.3：健康检测 + 故障接管
    // =====================================================================
    core::EventBus bus;

    gtw::HealthMonitorConfig hcfg;
    hcfg.heartbeat_interval   = core::DurationMs(5000);
    hcfg.suspect_after_misses = 1;
    hcfg.dead_after_misses    = 3;
    gtw::HealthMonitor hm(&bus, hcfg);

    auto make_node = [](gtw::NodeId id, std::uint32_t load) {
        gtw::NodeInfo info{};
        info.id   = id;
        info.addr = "10.0.0." + std::to_string(id);
        info.port = static_cast<std::uint16_t>(7000 + id);
        info.role = gtw::NodeRole::GameNode;
        info.load = load;
        return info;
    };
    (void)hm.Register(make_node(1, 100));  // 将死的节点
    (void)hm.Heartbeat(1, 100);

    // 检测：在心跳间隔 3 倍之后 Tick 一次，节点 1 应被判 Dead。
    const core::SteadyTime base = core::MonotonicClock::Point();
    const core::SteadyTime dead_at = base + core::DurationMs(16000);  // 3.2 个间隔
    const auto detect_start = core::MonotonicClock::Now();
    (void)hm.Tick(dead_at);
    const auto detect_end = core::MonotonicClock::Now();
    const double detect_ms = static_cast<double>(detect_end - detect_start) / 1e6;  // ns -> ms

    // 关键：替换节点必须在「检测 Tick」之后注册（仿 failover_test::KillNodeA），
    // 否则合成未来时间 dead_at 会让所有节点一起被判 Dead，找不到替换。
    (void)hm.Register(make_node(2, 10));   // 低负载替换候选
    (void)hm.Register(make_node(3, 200));  // 高负载候选
    (void)hm.Heartbeat(2, 10);
    (void)hm.Heartbeat(3, 200);

    // 故障接管：把节点 1 上的 players 个会话迁移到替换节点（带限速）。
    FakeSessionDir dir(1, players);
    FakeRebinder   rebinder;
    gtw::FailoverCoordinator fc(hm, dir, rebinder, /*max_concurrent=*/64);

    const auto fo_start = core::MonotonicClock::Now();
    auto fo = fc.OnNodeDead(1, /*trace=*/1);
    const auto fo_end = core::MonotonicClock::Now();
    const double fo_total_ms = static_cast<double>(fo_end - fo_start) / 1e6;  // ns -> ms
    const double reattach_ms_per_player = fo_total_ms / static_cast<double>(players);

    if (!fo.HasValue() || rebinder.count_ != players) {
        ErrorFmt("bench: failover did not rebind all %zu players (rebinder=%zu, ok=%d)\n",
                 players, rebinder.count_, fo.HasValue() ? 1 : 0);
    }

    // =====================================================================
    // 37.4：Scene Checkpoint + Restore
    // =====================================================================
    core::EventBus          sbus;
    game::EntityManager     entities(&sbus);
    core::Scheduler        scheduler;
    core::Arena             arena(1 << 16);
    game::SceneManager      smgr(entities, sbus, scheduler, arena);
    SceneManagerFactory     factory(smgr);
    rec::SceneRecovery      recovery(factory, &sbus);

    const game::SceneId sid = game::MakeSceneId(0, 7001);
    auto cr = smgr.Create(sid, game::SceneType::World, /*owner=*/1);
    if (!cr.HasValue()) { ErrorFmt("bench: create scene failed\n"); return; }
    game::Scene* s = cr.Value();
    (void)s->TransitionTo(game::SceneState::Loading);
    (void)s->TransitionTo(game::SceneState::Running);
    (void)s->ComputeStateHash();

    const auto cp_start = core::MonotonicClock::Now();
    (void)recovery.Checkpoint(*s);
    const auto cp_end = core::MonotonicClock::Now();
    const double checkpoint_ms = static_cast<double>(cp_end - cp_start) / 1e6;

    (void)smgr.Destroy(sid);

    const auto rs_start = core::MonotonicClock::Now();
    auto restored = recovery.Restore(sid, /*new_owner=*/2, /*trace=*/1);
    const auto rs_end = core::MonotonicClock::Now();
    const double scene_restore_ms = static_cast<double>(rs_end - rs_start) / 1e6;
    if (!restored.HasValue()) { ErrorFmt("bench: restore failed\n"); return; }
    (void)smgr.Destroy(sid);

    const std::size_t checkpoint_bytes = sizeof(rec::SceneCheckpoint);

    // ---- 输出：stdout + bench/resilience.txt ----
    Line("== TASK-037 resilience_bench ==\n");
    LineFmt("players=%zu\n", players);
    LineFmt("detect_ms=%.4f\n", detect_ms);
    LineFmt("reattach_ms_per_player=%.6f\n", reattach_ms_per_player);
    LineFmt("scene_restore_ms=%.6f\n", scene_restore_ms);
    LineFmt("checkpoint_ms=%.6f\n", checkpoint_ms);
    LineFmt("checkpoint_bytes=%zu\n", checkpoint_bytes);

    std::ofstream ofs("bench/resilience.txt");
    if (!ofs) { ErrorFmt("resilience_bench: cannot write bench/resilience.txt\n"); return; }
    ofs << "players=" << players << "\n";
    ofs << "detect_ms=" << detect_ms << "\n";
    ofs << "reattach_ms_per_player=" << reattach_ms_per_player << "\n";
    ofs << "scene_restore_ms=" << scene_restore_ms << "\n";
    ofs << "checkpoint_ms=" << checkpoint_ms << "\n";
    ofs << "checkpoint_bytes=" << checkpoint_bytes << "\n";
    ofs.close();

    LineFmt("resilience_bench done -> bench/resilience.txt\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t players = 1000;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--players") players = static_cast<std::size_t>(std::stoull(argv[i + 1]));
    }
    Run(players);
    return 0;
}
