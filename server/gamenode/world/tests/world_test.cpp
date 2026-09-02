// server/gamenode/world/tests/world_test.cpp — TASK-020 §16 单元 / §17 集成 / §19 Failure
//
// 覆盖：五状态机全路径与非法转移；创建/加入/退出/完成/销毁；超时回收；空实例回收；
// 全员退出回收；人数上限；重复加入拒绝；实例 ID 唯一；配置加载与校验（缺字段/未知类型报错）；
// 分线触发；TransferPlayer 目标满返回 BUSY 且玩家留原场景；100 实例并发创建+全部回收无泄漏；
// 单 Tick 分批回收上限 10（防尖峰）。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/movement/movement_system.h"
#include "mmo/game/ai/ai_system.h"
#include "mmo/game/world/instance_def.h"
#include "mmo/game/world/instance_manager.h"
#include "mmo/game/world/world_config.h"
#include "mmo/game/world/world_manager.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::world;
using namespace mmo::game::ai;
using namespace mmo::game::movement;

using core::MonotonicClock;
using core::SteadyTime;

using core::test::ErrorFmt;
using core::test::LineFmt;

constexpr const char* kWorldDir = "config/gameplay/world";
constexpr const char* kInvalidDir = "config/gameplay/world/invalid";

int g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

SteadyTime At(SteadyTime base, std::int64_t ms) { return base + core::DurationMs(ms); }

struct Harness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena;
    EntityManager mgr;
    std::unique_ptr<aoi::IAoi> aoi_owner;
    aoi::IAoi& aoi;
    movement::MovementSystem movement;
    AiSystem ai;
    SceneManager scenes;
    InstanceManager instances;
    WorldManager world;

    Harness()
        : arena(4 * 1024 * 1024),
          mgr(&bus),
          aoi_owner(aoi::CreateDynamicGridAoi(aoi::AoiConfig{})),
          aoi(*aoi_owner),
          movement(movement::MovementConfig{}, &aoi),
          ai(mgr, movement, aoi, scheduler),
          scenes(mgr, bus, scheduler, arena),
          instances(scenes, ai, mgr, bus, scheduler, arena, 1),
          world(scenes, instances, mgr, 1) {
        movement.BindAoi(aoi);
    }
};

// ---- 1. 五状态机全路径 ----
void test_state_machine() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    h.instances.SetReclaimPolicy(core::DurationMs(0), core::DurationMs(50));
    const SteadyTime base = MonotonicClock::Point();
(void)    h.instances.Tick(base);  // anchor now_

    auto c = h.instances.Create(1001, std::vector<PlayerId>{1, 2}, 0);
    CHECK(c.HasValue());
    const InstanceId id = c.Value();
    CHECK(h.instances.Find(id)->state == InstanceState::Pending);

    CHECK(h.instances.Start(id, 0).HasValue());
    CHECK(h.instances.Find(id)->state == InstanceState::Running);

    CHECK(h.instances.Complete(id, InstanceResult::Cleared, 0).HasValue());
    CHECK(h.instances.Find(id)->state == InstanceState::Completed);

    // 非法转移：Completed 后不能再 Complete / Start
    CHECK(!h.instances.Complete(id, InstanceResult::Cleared, 0).HasValue());
    CHECK(!h.instances.Start(id, 0).HasValue());
    CHECK(!h.instances.AddMember(id, 3, 0).HasValue());

    // Destroying 后才真正移除
    CHECK(h.instances.Destroy(id, 0).HasValue());
    CHECK(h.instances.Find(id) == nullptr);
}

// ---- 2. Create 后未 Start 直接 Destroy（无悬挂） ----
void test_destroy_before_start() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    auto c = h.instances.Create(1001, std::vector<PlayerId>{1}, 0);
    CHECK(c.HasValue());
    CHECK(h.instances.Destroy(c.Value(), 0).HasValue());
    CHECK(h.instances.Find(c.Value()) == nullptr);
    CHECK(h.scenes.Count() == 0);  // 从未创建 Scene
}

// ---- 3. 未知 def_id / 非法转移（Pending 不可加人） ----
void test_unknown_def_and_join_guard() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    CHECK(!h.instances.Create(99999, std::vector<PlayerId>{1}, 0).HasValue());
    auto c = h.instances.Create(1001, std::vector<PlayerId>{}, 0);
    CHECK(c.HasValue());
    // Pending 不可加人（仅 Loading/Running 可加入）
    CHECK(!h.instances.AddMember(c.Value(), 1, 0).HasValue());
}

// ---- 4. 成员上限 + 重复加入拒绝 ----
void test_member_limits() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    auto c = h.instances.Create(1002, std::vector<PlayerId>{}, 0);  // arena max 10
    CHECK(c.HasValue());
    CHECK(h.instances.Start(c.Value(), 0).HasValue());
    for (PlayerId p = 1; p <= 10; ++p) {
        CHECK(h.instances.AddMember(c.Value(), p, 0).HasValue());
    }
    // 第 11 个 -> BUSY
    CHECK(!h.instances.AddMember(c.Value(), 11, 0).HasValue());
    // 重复加入 -> INVALID_ARGUMENT
    CHECK(!h.instances.AddMember(c.Value(), 5, 0).HasValue());
}

// ---- 5. 实例 ID 唯一 ----
void test_id_unique() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    auto a = h.instances.Create(1001, std::vector<PlayerId>{}, 0);
    auto b = h.instances.Create(1001, std::vector<PlayerId>{}, 0);
    CHECK(a.HasValue() && b.HasValue());
    CHECK(a.Value() != b.Value());
    CHECK(a.Value() == 1 && b.Value() == 2);
}

// ---- 6. 配置加载校验（缺字段 / 未知类型报错） ----
void test_config_validation() {
    Harness h;
    CHECK(!h.instances.LoadConfig(kInvalidDir).HasValue());  // 未知类型 / max_players=0
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    CHECK(h.instances.Find(1001) == nullptr);  // 配置加载不自动建实例
    // 通过 LookupDef 间接确认：Create 合法 def 成功
    CHECK(h.instances.Create(1001, std::vector<PlayerId>{}, 0).HasValue());
    CHECK(h.world.LoadConfig(kWorldDir).HasValue());
    CHECK(h.world.GetOrCreateOpenWorld(1).HasValue());  // world def 1 存在
    CHECK(!h.world.GetOrCreateOpenWorld(999).HasValue());  // 未知 world def
}

// ---- 7. 空实例回收（创建后无人进入 > 超时） ----
void test_empty_instance_reclaim() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    h.instances.SetReclaimPolicy(core::DurationMs(50), core::DurationMs(50));
    const SteadyTime base = MonotonicClock::Point();
(void)    h.instances.Tick(base);
    auto c = h.instances.Create(1001, std::vector<PlayerId>{}, 0);  // 无成员
    CHECK(c.HasValue());
    CHECK(h.instances.Start(c.Value(), 0).HasValue());
    // 未进入（ever_entered=false），超过空实例超时 -> Destroying
(void)    h.instances.Tick(At(base, 100));
    CHECK(h.instances.Find(c.Value())->state == InstanceState::Destroying);
    // 下一 Tick 真正析构
(void)    h.instances.Tick(At(base, 100));
    CHECK(h.instances.Find(c.Value()) == nullptr);
    CHECK(h.scenes.Count() == 0);
}

// ---- 8. 全员退出回收（延迟宽限） ----
void test_all_left_reclaim() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    h.instances.SetReclaimPolicy(core::DurationMs(0), core::DurationMs(50));
    const SteadyTime base = MonotonicClock::Point();
(void)    h.instances.Tick(base);
    auto c = h.instances.Create(1001, std::vector<PlayerId>{1}, 0);  // 有成员 -> 已 entered
    CHECK(c.HasValue());
    CHECK(h.instances.Start(c.Value(), 0).HasValue());
    // 移除唯一成员 -> 全员退出 -> Destroying（宽限 50ms）
    CHECK(h.instances.RemoveMember(c.Value(), 1, LeaveReason::Logout, 0).HasValue());
    CHECK(h.instances.Find(c.Value())->state == InstanceState::Destroying);
    CHECK(h.instances.Find(c.Value())->destroy_at > base);  // 宽限未来时刻
    // 宽限未到不析构
(void)    h.instances.Tick(At(base, 10));
    CHECK(h.instances.Find(c.Value()) != nullptr);
    // 宽限到期析构
(void)    h.instances.Tick(At(base, 60));
    CHECK(h.instances.Find(c.Value()) == nullptr);
}

// ---- 9. 超时回收（time_limit 到期转 Completed） ----
void test_timeout_reclaim() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    h.instances.SetReclaimPolicy(core::DurationMs(0), core::DurationMs(50));
    const SteadyTime base = MonotonicClock::Point();
(void)    h.instances.Tick(base);
    auto c = h.instances.Create(1004, std::vector<PlayerId>{1}, 0);  // time_limit 100ms
    CHECK(c.HasValue());
    CHECK(h.instances.Start(c.Value(), 0).HasValue());
    // 在 100ms 内：仍 Running
(void)    h.instances.Tick(At(base, 50));
    CHECK(h.instances.Find(c.Value())->state == InstanceState::Running);
    // 超过 100ms：Completed
(void)    h.instances.Tick(At(base, 200));
    CHECK(h.instances.Find(c.Value())->state == InstanceState::Completed);
    // 宽限到期析构
(void)    h.instances.Tick(At(base, 260));
    CHECK(h.instances.Find(c.Value()) == nullptr);
}

// ---- 10. 单 Tick 分批回收上限 10（100 个同时超时） ----
void test_batch_reclaim_cap() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    h.instances.SetReclaimPolicy(core::DurationMs(0), core::DurationMs(50));
    const SteadyTime base = MonotonicClock::Point();
(void)    h.instances.Tick(base);
    constexpr std::size_t kN = 100;
    for (std::size_t i = 0; i < kN; ++i) {
        auto c = h.instances.Create(1004, std::vector<PlayerId>{(PlayerId)(i + 1)}, 0);
        CHECK(c.HasValue());
        CHECK(h.instances.Start(c.Value(), 0).HasValue());
    }
    CHECK(h.instances.Count() == kN);
    // 全部超时 -> Completed
(void)    h.instances.Tick(At(base, 200));
    CHECK(h.instances.CountByState(InstanceState::Completed) == kN);
    // 宽限到期：单 Tick 仅回收 10 个（防尖峰）
(void)    h.instances.Tick(At(base, 260));
    CHECK(h.instances.Count() == kN - 10);
    // 继续回收直到清空
    for (std::size_t guard = 0; h.instances.Count() > 0 && guard < 100; ++guard) {
(void)        h.instances.Tick(At(base, 260));
    }
    CHECK(h.instances.Count() == 0);
    CHECK(h.scenes.Count() == 0);  // 全部 Scene 回收
}

// ---- 11. 分线触发 + TransferPlayer BUSY（玩家留原场景） ----
void test_sharding_and_transfer_busy() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    CHECK(h.world.LoadConfig(kWorldDir).HasValue());
    WorldConfig cfg;
    cfg.sharding_threshold = 2;  // 阈值调小便于测试
    CHECK(h.world.Init(cfg).HasValue());

    const SteadyTime base = MonotonicClock::Point();
(void)    h.world.Tick(base);

    auto s1 = h.world.GetOrCreateOpenWorld(1);
    CHECK(s1.HasValue());
    const SceneId shardA = s1.Value();
    // 放入 2 名玩家（直接 Enter，附带 Avatar）
    for (PlayerId p : {PlayerId(1), PlayerId(2)}) {
        auto e = h.mgr.Create(EntityType::Player, shardA, Position{0, 0, 0, 0});
        CHECK(e.HasValue());
        CHECK(h.scenes.Find(shardA).Value()->Enter(p, e.Value()->Id()).HasValue());
    }
    CHECK(h.scenes.Find(shardA).Value()->PlayerCount() == 2);

    // 分线触发：A 已满 -> 新建分线 B
    auto s2 = h.world.GetOrCreateOpenWorld(1);
    CHECK(s2.HasValue());
    CHECK(s2.Value() != shardA);
    // 再次获取：B 未满 -> 返回 B（不建第三线）
    auto s3 = h.world.GetOrCreateOpenWorld(1);
    CHECK(s3.HasValue() && s3.Value() == s2.Value());

    // TransferPlayer 目标满 -> BUSY，玩家留原场景（A 人数不变）
    auto e3 = h.mgr.Create(EntityType::Player, s2.Value(), Position{0, 0, 0, 0});
    CHECK(e3.HasValue());
    CHECK(h.scenes.Find(s2.Value()).Value()->Enter(3, e3.Value()->Id()).HasValue());
    auto tr = h.world.TransferPlayer(3, s2.Value(), shardA, 0);
    CHECK(!tr.HasValue());
    CHECK(tr.Err().Code() == core::ErrorCode::BUSY);
    CHECK(h.scenes.Find(shardA).Value()->PlayerCount() == 2);  // A 未被写入

    // TransferPlayer 正常跨线（目标未满）
    auto ok = h.world.TransferPlayer(1, shardA, s2.Value(), 0);
    CHECK(ok.HasValue());
    CHECK(h.world.PlayerScene(1) == s2.Value());
    CHECK(h.scenes.Find(shardA).Value()->PlayerCount() == 1);
    CHECK(h.scenes.Find(s2.Value()).Value()->PlayerCount() == 2);
    h.mgr.FlushDeferred();
}

// ---- 12. 集成：100 个 5 人副本全部回收，Scene/实体归零（无泄漏） ----
void test_integration_no_leak() {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    h.instances.SetReclaimPolicy(core::DurationMs(0), core::DurationMs(20));
    const SteadyTime base = MonotonicClock::Point();
(void)    h.world.Tick(base);  // anchor + 驱动（此处无实例）
    const std::size_t monster_base = h.mgr.AliveCount();  // 创建前基线（应为 0）
    CHECK(monster_base == 0);

    const std::size_t kN = 100;
    const std::size_t kPer = 5;
    std::vector<InstanceId> ids;
    ids.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        std::vector<PlayerId> members;
        for (std::size_t m = 0; m < kPer; ++m) members.push_back((PlayerId)(i * kPer + m + 1));
        auto c = h.instances.Create(1001, members, 0);  // dungeon, 2 spawns
        CHECK(c.HasValue());
        CHECK(h.instances.Start(c.Value(), 0).HasValue());
        CHECK(h.instances.Find(c.Value())->state == InstanceState::Running);
        ids.push_back(c.Value());
    }
    CHECK(h.instances.Count() == kN);
    CHECK(h.scenes.Count() == kN);
    CHECK(h.mgr.AliveCount() > monster_base);  // 怪物已生成

    // 全员退出 -> 回收（批量 10/Tick）
    std::size_t removed = 0;
    for (InstanceId id : ids) {
        const Instance* pinst = h.instances.Find(id);
        CHECK(pinst != nullptr);
        const std::vector<PlayerId> members_snapshot(pinst->members);  // 快照，避免迭代中 erase 失效
        for (PlayerId p : members_snapshot) {
            CHECK(h.instances.RemoveMember(id, p, LeaveReason::Logout, 0).HasValue());
            ++removed;
        }
    }
    CHECK(removed == kN * kPer);
    // 驱动回收
    for (std::size_t guard = 0; h.instances.Count() > 0 && guard < 200; ++guard) {
(void)        h.world.Tick(At(base, 1000 + guard * 100));
    }
    h.mgr.FlushDeferred();  // 释放延迟销毁的怪物实体
    CHECK(h.instances.Count() == 0);
    CHECK(h.scenes.Count() == 0);
    CHECK(h.mgr.AliveCount() == monster_base);  // 无实体泄漏（回收后回到基线）
}

}  // namespace

int main() {
    LineFmt("== TASK-020 world_test ==\n");
    test_state_machine();
    test_destroy_before_start();
    test_unknown_def_and_join_guard();
    test_member_limits();
    test_id_unique();
    test_config_validation();
    test_empty_instance_reclaim();
    test_all_left_reclaim();
    test_timeout_reclaim();
    test_batch_reclaim_cap();
    test_sharding_and_transfer_busy();
    test_integration_no_leak();
    LineFmt("world_test done: fail=%d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
