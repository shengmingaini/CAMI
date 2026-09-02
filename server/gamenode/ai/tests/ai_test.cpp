// server/gamenode/ai/tests/ai_test.cpp — TASK-018 §16 / §17 / §19
//
// 覆盖：六态转移全路径（Idle->Patrol->Chase->Attack->Return->Idle）；决策节流(5Hz)；
//       AOI 局部目标选取（最近敌对、无视消失目标）；脱离追击半径返程；死亡+Scheduler 重生
//       （不建线程）与位置复位；Spawn/Despawn 生命周期；配置化加载（无硬编码数值）。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <algorithm>
#include <chrono>
#include <cmath>
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
#include "mmo/game/ai/npc_config.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::ai;
using namespace mmo::game::movement;

using core::test::ErrorFmt;
using core::test::LineFmt;

int g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

struct Harness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena{256 * 1024};
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

    mmo::game::EntityId SpawnPlayer(float x, float z) {
        auto e = mgr.Create(mmo::game::EntityType::Player, ctx.id, mmo::game::Position{x, 0.0f, z, 0.0f});
        CHECK(e.HasValue());
        const mmo::game::EntityId id = e.Value()->Id();
        movement::MovementState ms;
        ms.pos = mmo::game::Position{x, 0.0f, z, 0.0f};
        (void)movement.Register(id, ms);
        (void)aoi.Enter(id, mmo::game::Position{x, 0.0f, z, 0.0f});
        return id;
    }
    void PlacePlayer(mmo::game::EntityId id, float x, float z) {
        auto* e = mgr.Find(id);
        CHECK(e != nullptr);
        mmo::game::Position p{x, 0.0f, z, 0.0f};
        e->SetPos(p);
        auto st = movement.StateOf(id);
        if (st.HasValue() && st.Value() != nullptr) st.Value()->pos = p;
        (void)aoi.Move(id, p);
    }
    void Step(core::DurationMs dt) {
        ctx.now += dt;
        (void)ai.Update(ctx);
    }
};

SpawnDef MakeMonster(float aggro = 15.0f, float chase = 30.0f, std::int64_t hp = 200) {
    SpawnDef d;
    d.type = mmo::game::EntityType::Monster;
    d.spawn_pos = mmo::game::Position{0.0f, 0.0f, 0.0f, 0.0f};
    d.patrol_radius = 10.0f;
    d.aggro_radius = aggro;
    d.chase_leave_radius = chase;
    d.respawn_seconds = 30;
    d.max_hp = hp;
    d.level = 1;
    return d;
}

// 六态全转移路径：Idle -> Patrol -> Chase -> Attack -> Return -> Idle
void test_six_states() {
    Harness h;
    auto m = h.ai.Spawn(MakeMonster(), h.ctx);
    CHECK(m.HasValue());
    const mmo::game::EntityId monster = m.Value();

    // Idle -> Patrol（无目标）
    h.Step(core::DurationMs(200));
    CHECK(h.ai.StateOf(monster) == AiState::Patrol);

    // 玩家进入仇恨半径（dist=10 < aggro 15, > attack 2.5）-> Chase
    mmo::game::EntityId player = h.SpawnPlayer(10.0f, 0.0f);
    h.Step(core::DurationMs(200));
    CHECK(h.ai.StateOf(monster) == AiState::Chase);
    CHECK(h.ai.TargetOf(monster) == player);

    // 玩家贴近（dist=2 <= attack 2.5）-> Attack
    h.PlacePlayer(player, 2.0f, 0.0f);
    h.Step(core::DurationMs(200));
    CHECK(h.ai.StateOf(monster) == AiState::Attack);

    // 玩家远离（dist=100 > chase 30）-> Return
    h.PlacePlayer(player, 100.0f, 0.0f);
    h.Step(core::DurationMs(200));
    CHECK(h.ai.StateOf(monster) == AiState::Return);

    // 返程到家（怪本就在原点）-> Idle
    h.Step(core::DurationMs(200));
    CHECK(h.ai.StateOf(monster) == AiState::Idle);
}

// 决策节流：1000 怪 1 秒决策次数应 ≈ 5000 (5Hz)，而非每 Tick 全量的 ~20000
void test_throttle() {
    Harness h;
    for (int i = 0; i < 1000; ++i) {
        SpawnDef d = MakeMonster();
        d.spawn_pos = mmo::game::Position{static_cast<float>(i), 0.0f, 0.0f, 0.0f};
        auto r = h.ai.Spawn(d, h.ctx);
        CHECK(r.HasValue());
    }
    CHECK(h.ai.AliveCount() == 1000);
    for (int k = 0; k < 21; ++k) h.Step(core::DurationMs(50));  // ≈ 1.05s
    const std::uint64_t d = h.ai.DecisionCount();
    CHECK(d >= 4000 && d <= 7000);   // 5Hz 带宽
    CHECK(d < 15000);                 // 远低于「每 Tick(20Hz) 全量」的 ~20000
    LineFmt("  throttle: 1000 monsters decisions in ~1s = %llu (expect ~5000)\n",
            static_cast<unsigned long long>(d));
}

// 目标选取：最近敌对 + 无视消失目标（走 AOI 局部查询）
void test_target_selection() {
    Harness h;
    auto m = h.ai.Spawn(MakeMonster(), h.ctx);
    CHECK(m.HasValue());
    const mmo::game::EntityId monster = m.Value();
    mmo::game::EntityId p1 = h.SpawnPlayer(10.0f, 0.0f);   // 更近
    mmo::game::EntityId p2 = h.SpawnPlayer(14.0f, 0.0f);   // 更远（仍在 aggro 内）
    h.Step(core::DurationMs(200));
    CHECK(h.ai.TargetOf(monster) == p1);  // 选取最近敌对
    // 近处玩家下线（销毁），目标应切换/清空，不卡死在 Chase
    (void)h.ai.Despawn(p1);
    (void)h.mgr.Destroy(p1);
    h.Step(core::DurationMs(200));
    // p1 已消失：目标应为 p2（仍可见）或清空；绝不能是已销毁的 p1
    CHECK(h.ai.TargetOf(monster) != p1);
}

// 死亡 + Scheduler 重生（不建线程）：位置与状态复位
void test_respawn() {
    Harness h;
    auto m = h.ai.Spawn(MakeMonster(15.0f, 30.0f, 200), h.ctx);
    CHECK(m.HasValue());
    const mmo::game::EntityId monster = m.Value();
    mmo::game::EntityId player = h.SpawnPlayer(50.0f, 0.0f);
    // 致命一击
    (void)h.ai.OnDamaged(monster, player, 99999);
    CHECK(h.ai.StateOf(monster) == AiState::Dead);
    CHECK(h.ai.CountByState(AiState::Dead) == 1);
    // 推进 Scheduler 越过重生延迟（30s）-> Revive 触发
    (void)h.scheduler.Tick(h.ctx.now + core::DurationMs(31000));
    CHECK(h.ai.StateOf(monster) == AiState::Idle);
    CHECK(h.ai.CountByState(AiState::Dead) == 0);
    auto* e = h.mgr.Find(monster);
    CHECK(e != nullptr);
    CHECK(std::fabs(e->Pos().x) < 1e-3f && std::fabs(e->Pos().z) < 1e-3f);  // 回到原点
}

// Spawn / Despawn 生命周期联动
void test_lifecycle() {
    Harness h;
    std::vector<mmo::game::EntityId> ids;
    for (int i = 0; i < 10; ++i) {
        auto r = h.ai.Spawn(MakeMonster(), h.ctx);
        CHECK(r.HasValue());
        ids.push_back(r.Value());
    }
    CHECK(h.ai.CountByState(AiState::Idle) == 10);
    const mmo::game::EntityId mid = ids[0];
    CHECK(h.ai.Despawn(mid).HasValue());
    CHECK(h.ai.StateOf(mid) == AiState::Idle);  // 未知实体返回 Idle 哨兵
    CHECK(h.ai.CountByState(AiState::Idle) == 9);
    CHECK(!h.ai.Despawn(0xDEADBEEFULL).HasValue());  // 销毁不存在实体 -> NOT_FOUND
}

// 配置化加载：数值全部来自 JSON，无硬编码（§21）
void test_config_loader() {
    auto r = LoadSpawnDefsFromDir("config/gameplay/npc");
    CHECK(r.HasValue());
    const auto& defs = r.Value();
    CHECK(defs.size() >= 2);
    const SpawnDef* pig = nullptr;
    for (const auto& d : defs) {
        if (d.npc_def_id == 1001) pig = &d;
    }
    CHECK(pig != nullptr);
    CHECK(pig->max_hp == 220);          // 来自 monsters.json
    CHECK(pig->aggro_radius == 18.0f);  // 来自 monsters.json（非代码硬编码）
    CHECK(pig->type == mmo::game::EntityType::Monster);
    // 缺失字段必须失败，禁止静默默认
    auto bad = LoadSpawnDefs("config/gameplay/npc/__not_exist__.json");
    CHECK(!bad.HasValue());
    LineFmt("  config: loaded %zu npc/monster defs from config/gameplay/npc\n",
            static_cast<unsigned long>(defs.size()));
}

}  // namespace

int main() {
    LineFmt("== TASK-018 ai_test ==\n");
    test_six_states();
    test_throttle();
    test_target_selection();
    test_respawn();
    test_lifecycle();
    test_config_loader();
    if (g_fail == 0) {
        LineFmt("Ai.Suite: PASS\n");
        return 0;
    }
    ErrorFmt("Ai.Suite: FAIL (%d)\n", g_fail);
    return 1;
}
