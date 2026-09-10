// server/gamenode/scene/tests/scene_recovery_test.cpp — TASK-037 §16 / §37.4 单元
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
// 第一版 Scene 恢复：Checkpoint 采集 + 新节点重建（不做 Live Migration）。

#include <cstdint>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/recovery/scene_recovery.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/scene/scene_manager.h"

namespace {

using namespace mmo::game;
using namespace mmo::game::scene::recovery;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;

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

// ---- 共享系统（仿 scene_test 的 harness） ----
core::EventBus& Bus() { static core::EventBus b; return b; }
EntityManager& Mgr() { static EntityManager m(&Bus()); return m; }
core::Scheduler& Sch() { static core::Scheduler s; return s; }
core::Arena& Ara() { static core::Arena a(1 << 16); return a; }

SceneManager& SMgr() {
    static SceneManager mgr(Mgr(), Bus(), Sch(), Ara());
    return mgr;
}

// 工厂窄接口适配：把 SceneManager 暴露给 SceneRecovery（§27.3 消费公开 Create）。
class SceneManagerFactory final : public ISceneFactory {
public:
    explicit SceneManagerFactory(SceneManager& m) : mgr_(m) {}
    core::Result<Scene*> Create(SceneId id, SceneType t, NodeId owner) override {
        return mgr_.Create(id, t, owner);
    }
private:
    SceneManager& mgr_;
};

Scene* MakeRunning(SceneId id, SceneType t, NodeId owner) {
    auto r = SMgr().Create(id, t, owner);
    if (!r.HasValue()) return nullptr;
    Scene* s = r.Value();
    (void)s->TransitionTo(SceneState::Loading);
    (void)s->TransitionTo(SceneState::Running);
    return s;
}

// ---------------------------------------------------------------------------
// §16 / §37.4 Checkpoint 捕获 + 元数据正确
// ---------------------------------------------------------------------------
void TestCheckpointCapturesState() {
    const SceneId id = MakeSceneId(0, 5001);
    auto* s = MakeRunning(id, SceneType::World, /*owner=*/1);
    CHECK(s != nullptr, "scene created");

    // 进入 3 名玩家并算一次哈希，制造“最近状态”
    std::vector<EntityId> aids;
    for (std::uint64_t p = 0; p < 3; ++p) {
        auto av = Mgr().Create(EntityType::Player, id, Position{});
        aids.push_back(av.Value()->Id());
        CHECK(s->Enter(9000 + p, aids.back()).HasValue(), "enter player");
    }
    CHECK(s->ComputeStateHash().HasValue(), "hash compute");
    const std::uint64_t hash = s->StateHash();

    SceneManagerFactory factory(SMgr());
    SceneRecovery rec(factory, &Bus());

    CHECK(rec.Checkpoint(*s).HasValue(), "checkpoint ok");
    CHECK(rec.LastCheckpointVersion(id) == 1, "first checkpoint version == 1");
    CHECK(rec.CheckpointCount() == 1, "one checkpoint stored");

    // 崩溃模拟：销毁原 Scene（旧节点下线）
    Mgr().FlushDeferred();
    CHECK(SMgr().Destroy(id).HasValue(), "destroy original scene (crash)");

    // 新节点恢复：身份一致，Owner 变为新节点，玩家回退（待重连补回）
    auto restored = rec.Restore(id, /*new_owner=*/2, /*trace=*/1);
    CHECK(restored.HasValue(), "restore ok");
    Scene* r = restored.Value();
    CHECK(r->Id() == id, "restored id matches");
    CHECK(r->Type() == SceneType::World, "restored type matches");
    CHECK(r->OwnerNode() == 2, "restored owner is new node");
    CHECK(r->State() == SceneState::Running, "restored scene is Running");
    CHECK(r->PlayerCount() == 0, "players rolled back (re-enter via failover)");
    CHECK(r->TickNumber() == 0, "fresh tick number after restore");

    // checkpoint 元数据仍可供对账（未因恢复而改变）
    CHECK(rec.LastCheckpointVersion(id) == 1, "checkpoint version stable after restore");

    for (auto a : aids) (void)Mgr().Destroy(a);
    Mgr().FlushDeferred();
    (void)SMgr().Destroy(id);
}

// ---------------------------------------------------------------------------
// §16 / §37.4 多次 Checkpoint 序号递增
// ---------------------------------------------------------------------------
void TestCheckpointVersionIncrements() {
    const SceneId id = MakeSceneId(0, 5002);
    auto* s = MakeRunning(id, SceneType::Dungeon, 3);
    CHECK(s != nullptr, "scene created 2");

    SceneManagerFactory factory(SMgr());
    SceneRecovery rec(factory, &Bus());

    CHECK(rec.Checkpoint(*s).HasValue(), "cp1");
    CHECK(rec.Checkpoint(*s).HasValue(), "cp2");
    CHECK(rec.Checkpoint(*s).HasValue(), "cp3");
    CHECK(rec.LastCheckpointVersion(id) == 3, "version increments to 3");

    CHECK(SMgr().Destroy(id).HasValue(), "destroy 2");
}

// ---------------------------------------------------------------------------
// §16 / §37.4 无 checkpoint 时 Restore 失败（NOT_FOUND）
// ---------------------------------------------------------------------------
void TestRestoreWithoutCheckpointFails() {
    const SceneId id = MakeSceneId(0, 5003);
    SceneManagerFactory factory(SMgr());
    SceneRecovery rec(factory, &Bus());
    auto r = rec.Restore(id, 9, 1);
    CHECK(!r.HasValue(), "restore without checkpoint fails");
    if (!r.HasValue()) {
        CHECK(r.Err().Code() == ErrorCode::NOT_FOUND, "NOT_FOUND code");
    }
}

// ---------------------------------------------------------------------------
// §19 Failure：Checkpoint 写入失败 → 保留上一个 checkpoint 并告警（不覆盖）
// ---------------------------------------------------------------------------
void TestCheckpointFailureKeepsLastGood() {
    const SceneId id = MakeSceneId(0, 5004);
    auto* s = MakeRunning(id, SceneType::Arena, 4);
    CHECK(s != nullptr, "scene created 3");

    SceneManagerFactory factory(SMgr());
    SceneRecovery rec(factory, &Bus());

    CHECK(rec.Checkpoint(*s).HasValue(), "first good checkpoint");
    CHECK(rec.LastCheckpointVersion(id) == 1, "v1 recorded");

    // 注入下一次 Checkpoint 失败
    rec.FailNextCheckpoint(true);
    auto bad = rec.Checkpoint(*s);
    CHECK(!bad.HasValue(), "injected checkpoint fails");
    if (!bad.HasValue()) {
        CHECK(bad.Err().Code() == ErrorCode::INTERNAL_ERROR, "INTERNAL_ERROR code");
    }
    // 上一个 checkpoint 必须保留（不覆盖、不污染）
    CHECK(rec.LastCheckpointVersion(id) == 1, "last good checkpoint preserved");
    CHECK(rec.CheckpointCount() == 1, "store unchanged after failed write");

    // 恢复仍可用（基于最后一个好 checkpoint）
    CHECK(SMgr().Destroy(id).HasValue(), "destroy 3");
    auto restored = rec.Restore(id, 5, 1);
    CHECK(restored.HasValue(), "restore still works from last good checkpoint");

    (void)SMgr().Destroy(id);
}

}  // namespace

int main() {
    Line("== TASK-037 scene recovery (37.4) test ==\n");
    TestCheckpointCapturesState();
    TestCheckpointVersionIncrements();
    TestRestoreWithoutCheckpointFails();
    TestCheckpointFailureKeepsLastGood();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
