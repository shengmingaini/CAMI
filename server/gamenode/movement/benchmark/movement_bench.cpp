// server/gamenode/movement/benchmark/movement_bench.cpp — TASK-015 §18
//
// 输出机器可读 key=value 到 bench/movement.txt（验收脚本 assert_metric 解析）：
//   move_ns_per_entity / validate_ns / integrate_ns_per_1k / aoi_update_ns
//
// 设计：move_ns_per_entity 测量 MovementSystem::ApplyCommand 核心（校验 + 状态更新 +
// 写实体 + 事件），**不绑定 AOI**（AOI 联动成本单独计入 aoi_update_ns），对应 Movement
// 阶段预算（§22）；AOI 是调度器中独立阶段（TASK-013 八阶段顺序）。

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
#include <vector>

#include "test_print.h"

#include "mmo/core/time/clock.h"                 // mmo::core::SteadyNs / MonotonicClock
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/aoi/dynamic_grid_aoi.h"
#include "mmo/game/movement/movement_state.h"
#include "mmo/game/movement/movement_system.h"
#include "mmo/game/movement/validator.h"

using mmo::core::SteadyNs;
using mmo::core::MonotonicClock;
// 本文件位于全局作用域，mmo::core / mmo::game 均为 mmo 的嵌套命名空间，
// 非限定名 core:: / aoi:: 不会自动解析，必须显式引入。
namespace core = mmo::core;
namespace aoi = mmo::game::aoi;
using namespace mmo::game;              // EntityManager / SceneType / SceneContext
using namespace mmo::game::movement;

namespace {
MoveCommand MakeCmd(EntityId id, const Position& from, const Position& to,
                    std::uint32_t seq, std::int64_t ts) {
    MoveCommand c;
    c.entity = id;
    c.from = from;
    c.to = to;
    c.client_seq = seq;
    c.client_timestamp_ms = ts;
    return c;
}
}  // namespace

int main(int argc, char** argv) {
    std::size_t entities = 1000;
    std::size_t ticks = 10000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--entities" && i + 1 < argc) entities = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (a == "--ticks" && i + 1 < argc) ticks = static_cast<std::size_t>(std::atoll(argv[++i]));
    }

    constexpr std::uint64_t kScene = 1;
    const float step = 6.0f * 0.05f;  // max_speed*dt

    // ---- 世界：实体 + 移动状态（不绑 AOI，隔离 Movement 成本） ----
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena{1 << 20};
    mmo::game::EntityManager mgr{&bus};
    SceneContext ctx(kScene, SceneType::World, 1,
                     MonotonicClock::Point(), 0, mgr, bus, scheduler, arena);

    MovementSystem ms;  // 默认配置，不绑 AOI
    aoi::AoiConfig acfg;
    // 变量名禁止叫 aoi：声明点在初始化器之前，会遮蔽 aoi 命名空间。
    auto aoiInst = aoi::CreateDynamicGridAoi(acfg);

    std::mt19937 rng(20260831);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);

    std::vector<EntityId> ids(entities);
    std::vector<Position> pos(entities);
    std::vector<std::uint32_t> seq(entities, 0);
    for (std::size_t i = 0; i < entities; ++i) {
        Position p{d(rng) * 1e3f, d(rng) * 1e3f, d(rng) * 1e3f, 0};
        auto er = mgr.Create(mmo::game::EntityType::Player, kScene, p);
        ids[i] = er.Value()->Id();
        pos[i] = p;
        MovementState st;
        st.pos = p;
        (void)ms.Register(ids[i], st);
        (void)aoiInst->Enter(ids[i], p);  // 供 aoi_update_ns 测量
    }

    // ---- 1. move_ns_per_entity：ApplyCommand 核心（不绑 AOI） ----
    const SteadyNs t0 = MonotonicClock::Now();
    for (std::size_t t = 0; t < ticks; ++t) {
        ctx.tick_number = t;
        for (std::size_t i = 0; i < entities; ++i) {
            Position to = pos[i];
            to.x += d(rng) * step * 0.9f;
            to.y += d(rng) * step * 0.9f;
            to.z += d(rng) * step * 0.9f;
            auto r = ms.ApplyCommand(MakeCmd(ids[i], pos[i], to, ++seq[i],
                                             static_cast<std::int64_t>((t + 1) * 40)), ctx);
            if (r.HasValue()) pos[i] = ms.StateOf(ids[i]).Value()->pos;
        }
    }
    const SteadyNs t1 = MonotonicClock::Now();
    const double move_ns_per_entity =
        static_cast<double>(t1 - t0) / static_cast<double>(ticks * entities);

    // ---- 2. validate_ns：纯校验函数（无状态突变 / 无发布） ----
    MovementState vst;
    vst.pos = Position{0, 0, 0, 0};
    MoveCommand vcmd = MakeCmd(1, Position{0, 0, 0, 0}, Position{step * 0.9f, 0, 0, 0}, 1, 100);
    const std::size_t viter = entities * 20;
    const SteadyNs vt0 = MonotonicClock::Now();
    for (std::size_t k = 0; k < viter; ++k) {
        volatile MoveReject r = ValidateMovement(vcmd, vst, ms.GetConfig());
        (void)r;
    }
    const SteadyNs vt1 = MonotonicClock::Now();
    const double validate_ns = static_cast<double>(vt1 - vt0) / static_cast<double>(viter);

    // ---- 3. integrate_ns_per_1k：1000 实体带速度积分一 Tick ----
    double integrate_ns_per_1k = 0.0;
    {
        MovementSystem ms2;
        mmo::game::SceneContext ctx2(kScene, mmo::game::SceneType::World, 1,
                                             MonotonicClock::Point(), 1, mgr, bus, scheduler, arena);
        const std::size_t K = (entities >= 1000) ? 1000 : entities;
        std::vector<EntityId> kids(K);
        for (std::size_t i = 0; i < K; ++i) {
            auto er = mgr.Create(mmo::game::EntityType::Player, kScene, pos[i % entities]);
            kids[i] = er.Value()->Id();
            MovementState st;
            st.pos = pos[i % entities];
            (void)ms2.Register(kids[i], st);
            (void)ms2.SetVelocity(kids[i], Vec3{step / 0.05f, 0, 0});  // ~6 m/s
        }
        const SteadyNs it0 = MonotonicClock::Now();
        (void)ms2.Integrate(ctx2, 0.05f);  // tick_number=1 != moved_tick(0) → 推进
        const SteadyNs it1 = MonotonicClock::Now();
        integrate_ns_per_1k = static_cast<double>(it1 - it0) / static_cast<double>(K);
    }

    // ---- 4. aoi_update_ns：AOI 联动（iaoi::Move）每实体成本 ----
    ms.BindAoi(*aoiInst);
    const SteadyNs at0 = MonotonicClock::Now();
    for (std::size_t i = 0; i < entities; ++i) {
        (void)aoiInst->Move(ids[i], pos[i]);
    }
    const SteadyNs at1 = MonotonicClock::Now();
    const double aoi_update_ns = static_cast<double>(at1 - at0) / static_cast<double>(entities);

    // ---- 输出 ----
    std::FILE* fp = std::fopen("bench/movement.txt", "w");
    if (fp) {
        std::fprintf(fp,
                     "entities=%zu\n"
                     "ticks=%zu\n"
                     "move_ns_per_entity=%.3f\n"
                     "validate_ns=%.3f\n"
                     "integrate_ns_per_1k=%.3f\n"
                     "aoi_update_ns=%.3f\n",
                     entities, ticks, move_ns_per_entity, validate_ns, integrate_ns_per_1k, aoi_update_ns);
        std::fclose(fp);
    }
    mmo::core::test::LineFmt(
        "entities=%zu ticks=%zu move_ns_per_entity=%.3f validate_ns=%.3f "
        "integrate_ns_per_1k=%.3f aoi_update_ns=%.3f\n",
        entities, ticks, move_ns_per_entity, validate_ns, integrate_ns_per_1k, aoi_update_ns);
    return 0;
}
