// server/gamenode/ai/benchmark/ai_bench.cpp — TASK-018 §18 / §22 / §24
//
// 输出机器可读 key=value 到 bench/ai.txt（验收脚本 assert_metric 解析）：
//   monsters / ticks / ai_update_ns_per_entity / decisions_per_second_per_1k /
//   mem_bytes_per_ai / ai_phase_us_at_1k
//
// 口径：1000 个 AI 实体在 Scene 中长稳驱动（决策节流 5Hz，目标选取走 AOI 局部查询，
//       Chase/Patrol 经 Movement 积分推进坐标），统计单 Tick 的 AI 阶段耗时。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/movement/movement_system.h"
#include "mmo/game/ai/ai_system.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::ai;
using namespace mmo::game::movement;

using core::MonotonicClock;
using core::SteadyNs;
using core::test::ErrorFmt;
using core::test::LineFmt;

int g_bench_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++g_bench_fail;                                                  \
        }                                                                    \
    } while (0)

struct Harness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena{4 * 1024 * 1024};
    EntityManager mgr{&bus};
    std::unique_ptr<aoi::IAoi> aoi_owner;
    aoi::IAoi& aoi;
    movement::MovementSystem movement;
    AiSystem ai;
    SceneContext ctx;

    Harness()
        : aoi_owner(aoi::CreateDynamicGridAoi(aoi::AoiConfig{})),
          aoi(*aoi_owner),
          movement(movement::MovementConfig{}, &aoi),
          ai(mgr, movement, aoi, scheduler),
          ctx(7, SceneType::World, 1, core::MonotonicClock::Point(), 0,
              mgr, bus, scheduler, arena) {
        movement.BindAoi(aoi);
    }
};

void Run(std::size_t monsters, std::size_t ticks, const char* out_path) {
    Harness h;

    // ---- 生成 monsters：50x20 网格，间距 5m ----
    for (std::size_t i = 0; i < monsters; ++i) {
        SpawnDef d;
        d.type = mmo::game::EntityType::Monster;
        d.spawn_pos = mmo::game::Position{static_cast<float>(i % 50) * 5.0f, 0.0f,
                                       static_cast<float>(i / 50) * 5.0f, 0.0f};
        d.patrol_radius = 10.0f;
        d.aggro_radius = 15.0f;
        d.chase_leave_radius = 30.0f;
        d.respawn_seconds = 30;
        d.max_hp = 200;
        d.level = 1;
        auto r = h.ai.Spawn(d, h.ctx);
        CHECK(r.HasValue());
    }

    // ---- 生成 50 个玩家，散布在怪物群中（部分怪会进入 Chase/Attack） ----
    const std::size_t players = 50;
    for (std::size_t j = 0; j < players; ++j) {
        const std::size_t base = (j * 20) % monsters;
        const float x = static_cast<float>(base % 50) * 5.0f + 3.0f;
        const float z = static_cast<float>(base / 50) * 5.0f + 3.0f;
        auto e = h.mgr.Create(mmo::game::EntityType::Player, h.ctx.id,
                              mmo::game::Position{x, 0.0f, z, 0.0f});
        CHECK(e.HasValue());
        const mmo::game::EntityId id = e.Value()->Id();
        movement::MovementState ms;
        ms.pos = mmo::game::Position{x, 0.0f, z, 0.0f};
        (void)h.movement.Register(id, ms);
        (void)h.aoi.Enter(id, mmo::game::Position{x, 0.0f, z, 0.0f});
    }

    CHECK(h.ai.AliveCount() == monsters);

    // ---- 长稳驱动 ----
    const float dt = 0.05f;  // 20Hz
    SteadyNs ai_ns = 0;
    for (std::size_t t = 0; t < ticks; ++t) {
        h.ctx.now += core::DurationMs(50);
        h.ctx.tick_number = t;
        const SteadyNs t0 = MonotonicClock::Now();
        (void)h.ai.Update(h.ctx);
        const SteadyNs t1 = MonotonicClock::Now();
        ai_ns += (t1 - t0);
        (void)h.movement.Integrate(h.ctx, dt);  // 推进坐标 + 同步 AOI
        (void)h.scheduler.Tick(h.ctx.now);
    }

    const double elapsed_s = static_cast<double>(ticks) * dt;
    const double per_tick_ns = static_cast<double>(ai_ns) / static_cast<double>(ticks);
    const double ai_phase_us_at_1k = per_tick_ns / 1000.0;            // 1000 实体的 AI 阶段耗时(us)
    const double ai_update_ns_per_entity = per_tick_ns / static_cast<double>(monsters);
    const double decisions_per_second_per_1k =
        static_cast<double>(h.ai.DecisionCount()) / elapsed_s;        // 1000 实体≈此值
    const double mem_bytes_per_ai = static_cast<double>(sizeof(AiComponent));

    std::FILE* fp = std::fopen(out_path, "w");
    if (fp == nullptr) { ErrorFmt("FAIL: cannot open %s\n", out_path); std::exit(1); }
    std::fprintf(fp,
                 "monsters=%zu\n"
                 "ticks=%zu\n"
                 "ai_update_ns_per_entity=%.3f\n"
                 "decisions_per_second_per_1k=%.3f\n"
                 "mem_bytes_per_ai=%.3f\n"
                 "ai_phase_us_at_1k=%.3f\n",
                 monsters, ticks, ai_update_ns_per_entity,
                 decisions_per_second_per_1k, mem_bytes_per_ai, ai_phase_us_at_1k);
    std::fclose(fp);

    LineFmt("monsters=%zu ticks=%zu ai_update_ns_per_entity=%.3f "
             "decisions_per_second_per_1k=%.3f mem_bytes_per_ai=%.3f "
             "ai_phase_us_at_1k=%.3f\n",
             monsters, ticks, ai_update_ns_per_entity, decisions_per_second_per_1k,
             mem_bytes_per_ai, ai_phase_us_at_1k);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t monsters = 1000;
    std::size_t ticks = 12000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--monsters" && i + 1 < argc) monsters = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (a == "--ticks" && i + 1 < argc) ticks = static_cast<std::size_t>(std::atoll(argv[++i]));
    }
    LineFmt("== TASK-018 ai_bench ==\n");
    Run(monsters, ticks, "bench/ai.txt");
    return g_bench_fail == 0 ? 0 : 1;
}
