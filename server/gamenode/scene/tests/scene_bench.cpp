// server/gamenode/scene/tests/scene_bench.cpp — TASK-012 §18 Benchmark
//
// 指标（机器可读 key=value，供验收脚本 assert_metric 解析）：
//   scene_tick_overhead_ns=          单场景单 Tick 平均开销（ns），§22 < 1000
//   enter_ns=                        单次 Enter 平均开销（ns），§22 < 5000
//   leave_ns=                        单次 Leave 平均开销（ns），§22 < 5000
//   state_hash_us_per_1k_entities=   1000 实体 StateHash 耗时（us），§22 < 200
//   mem_bytes_per_scene=             sizeof(Scene) 基础内存（B），§22 < 65536
//
// 用法：scene_bench [--scenes N] [--ticks T]   默认 scenes=100 ticks=1000
// 输出：bench/scene.txt

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/scene/scene_manager.h"

namespace {

using namespace mmo::game;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
namespace core = mmo::core;

}  // namespace

int main(int argc, char** argv) {
    std::size_t scenes = 100;
    std::size_t ticks = 1000;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scenes") scenes = static_cast<std::size_t>(std::stoull(argv[i + 1]));
        else if (a == "--ticks") ticks = static_cast<std::size_t>(std::stoull(argv[i + 1]));
    }

    core::EventBus bus;
    EntityManager entities(&bus);
    core::Scheduler scheduler;
    core::Arena arena(1 << 16);
    SceneManager mgr(entities, bus, scheduler, arena);

    // ---- 准备：N 个 Running 场景，每场景 8 名玩家（Avatar 经 EntityManager 创建）----
    constexpr std::size_t kPlayersPerScene = 8;
    for (std::size_t i = 0; i < scenes; ++i) {
        const SceneId id = MakeSceneId(0, 5000 + i);
        auto r = mgr.Create(id, SceneType::World, 1);
        if (!r.HasValue()) { ErrorFmt("bench: create scene failed\n"); return 1; }
        Scene* s = r.Value();
        (void)s->TransitionTo(SceneState::Loading);
        (void)s->TransitionTo(SceneState::Running);
        for (std::size_t p = 0; p < kPlayersPerScene; ++p) {
            auto av = entities.Create(EntityType::Player, id, Position{});
            if (!av.HasValue()) { ErrorFmt("bench: create avatar failed\n"); return 1; }
            if (!s->Enter(100 + p, av.Value()->Id()).HasValue()) {
                ErrorFmt("bench: enter failed\n"); return 1;
            }
        }
    }

    // ---- 1. Tick 框架开销（§22 < 1000ns）----
    {
        const auto t0 = core::MonotonicClock::Now();
        for (std::size_t t = 0; t < ticks; ++t) {
            (void)mgr.TickAll(core::MonotonicClock::Point());
        }
        const double total_ns = static_cast<double>(core::MonotonicClock::Now() - t0);
        const double per = total_ns / static_cast<double>(scenes * ticks);

        // ---- 2. Enter 开销（§22 < 5000ns）----
        const SceneId eid = MakeSceneId(0, 9999);
        auto er = mgr.Create(eid, SceneType::World, 1);
        Scene* es = er.Value();
        (void)es->TransitionTo(SceneState::Loading);
        (void)es->TransitionTo(SceneState::Running);
        constexpr std::size_t kM = 2000;
        std::vector<EntityId> eavs;
        eavs.reserve(kM);
        for (std::size_t i = 0; i < kM; ++i) {
            auto av = entities.Create(EntityType::Player, eid, Position{});
            eavs.push_back(av.Value()->Id());
        }
        const auto te = core::MonotonicClock::Now();
        for (std::size_t i = 0; i < kM; ++i) {
            (void)es->Enter(5000 + i, eavs[i]);
        }
        const double enter_ns = static_cast<double>(core::MonotonicClock::Now() - te) /
                               static_cast<double>(kM);

        // ---- 3. Leave 开销（§22 < 5000ns）----
        const auto tl = core::MonotonicClock::Now();
        for (std::size_t i = 0; i < kM; ++i) {
            (void)es->Leave(5000 + i, LeaveReason::Logout);
        }
        const double leave_ns = static_cast<double>(core::MonotonicClock::Now() - tl) /
                               static_cast<double>(kM);

        (void)mgr.Destroy(eid);
        entities.FlushDeferred();

        // ---- 4. StateHash：1000 实体（§22 < 200us）----
        const SceneId hid = MakeSceneId(0, 8888);
        auto hr = mgr.Create(hid, SceneType::World, 1);
        Scene* hs = hr.Value();
        (void)hs->TransitionTo(SceneState::Loading);
        (void)hs->TransitionTo(SceneState::Running);
        constexpr std::size_t kH = 1000;
        for (std::size_t i = 0; i < kH; ++i) {
            auto av = entities.Create(EntityType::Player, hid, Position{});
            (void)hs->Enter(7000 + i, av.Value()->Id());
        }
        const auto th = core::MonotonicClock::Now();
        (void)hs->ComputeStateHash();
        const double hash_us = static_cast<double>(core::MonotonicClock::Now() - th) / 1000.0;
        (void)mgr.Destroy(hid);
        entities.FlushDeferred();

        // ---- 5. 基础内存 ----
        const std::size_t mem = sizeof(Scene);

        Line("== TASK-012 scene_bench ==\n");
        LineFmt("scenes=%zu\n", scenes);
        LineFmt("ticks=%zu\n", ticks);
        LineFmt("scene_tick_overhead_ns=%.3f\n", per);
        LineFmt("enter_ns=%.3f\n", enter_ns);
        LineFmt("leave_ns=%.3f\n", leave_ns);
        LineFmt("state_hash_us_per_1k_entities=%.3f\n", hash_us);
        LineFmt("mem_bytes_per_scene=%zu\n", mem);

        std::ofstream ofs("bench/scene.txt");
        if (!ofs) { ErrorFmt("scene_bench: cannot write bench/scene.txt\n"); return 1; }
        ofs << "scenes=" << scenes << "\n";
        ofs << "ticks=" << ticks << "\n";
        ofs << "scene_tick_overhead_ns=" << per << "\n";
        ofs << "enter_ns=" << enter_ns << "\n";
        ofs << "leave_ns=" << leave_ns << "\n";
        ofs << "state_hash_us_per_1k_entities=" << hash_us << "\n";
        ofs << "mem_bytes_per_scene=" << mem << "\n";
        ofs.close();

        LineFmt("scene_bench done -> bench/scene.txt\n");
    }
    return 0;
}
