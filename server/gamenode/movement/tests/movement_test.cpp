// server/gamenode/movement/tests/movement_test.cpp — TASK-015 §16 / §17 / §19
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
// ctest 标签：Movement（验收脚本 run_ctest 'Movement'）。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "test_print.h"

#include "mmo/core/error/error_code.h"            // ErrorCode
#include "mmo/core/memory/arena.h"               // core::Arena
#include "mmo/core/sched/scheduler.h"            // core::Scheduler
#include "mmo/core/time/clock.h"                 // core::SteadyTime / MonotonicClock
#include "mmo/game/entity/entity.h"              // EntityType / Position
#include "mmo/game/entity/entity_id.h"           // EntityId
#include "mmo/game/entity/entity_manager.h"       // EntityManager
#include "mmo/game/scene/scene_context.h"         // SceneContext
#include "mmo/game/aoi/aoi.h"                     // aoi::IAoi / CreateDynamicGridAoi
#include "mmo/game/aoi/dynamic_grid_aoi.h"
#include "mmo/game/movement/movement_state.h"     // MovementState / MoveCommand / MoveReject
#include "mmo/game/movement/movement_system.h"    // MovementSystem
#include "mmo/game/movement/validator.h"          // ValidateMovement

namespace {

// 注：本文件位于全局匿名命名空间，mmo::core / mmo::game 均为 mmo 的嵌套命名空间，
// 非限定名 core:: / aoi:: 不会自动解析，必须显式引入（禁止裸写 using namespace mmo;）。
namespace core = mmo::core;            // core::EventBus / Arena / Scheduler / MonotonicClock
using namespace mmo::game;             // EntityManager / Position / EntityId / SceneContext / aoi::
using namespace mmo::game::movement;   // MovementSystem / MovementState / Vec3 ...

using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
using mmo::core::ErrorCode;

int g_fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            ++g_fails;                                                          \
        }                                                                       \
    } while (0)
#define CHECK_CODE(result, expected, msg)                                       \
    do {                                                                        \
        const auto& r_ = (result);                                              \
        if (r_.HasValue()) {                                                    \
            ErrorFmt("FAIL: %s (line %d): expected error, got ok\n", msg, __LINE__); \
            ++g_fails;                                                          \
        } else if (r_.Err().Code() != (expected)) {                            \
            ErrorFmt("FAIL: %s (line %d): code mismatch\n", msg, __LINE__);     \
            ++g_fails;                                                          \
        }                                                                       \
    } while (0)

constexpr float kWorld = 1.0e6f;
constexpr std::uint64_t kScene = 1;

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

// 构造一个最小 SceneContext（bus/scheduler/arena 均为测试本地实例）。
struct TestHarness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena{256 * 1024};
    mmo::game::EntityManager mgr{&bus};
    SceneContext ctx;

    TestHarness()
        : ctx(kScene, mmo::game::SceneType::World, 1,
              core::MonotonicClock::Point(), 0, mgr, bus, scheduler, arena) {}
};

// ---------------------------------------------------------------------------
// 1. 五条校验规则：触发与不触发
// ---------------------------------------------------------------------------
void TestValidateRules() {
    MovementConfig cfg;  // 默认 max_speed=6, dt=0.05, world_half=1e6
    MovementState st;
    st.pos = Position{0, 0, 0, 0};
    st.max_speed = cfg.max_speed;

    const float step = cfg.max_speed * cfg.tick_dt;            // 0.3m
    const float too_fast = step * cfg.speed_tolerance;          // 0.345m
    const float teleport = step * cfg.teleport_factor;          // 0.9m

    CHECK(ValidateMovement(MakeCmd(1, st.pos, Position{too_fast * 0.9f, 0, 0, 0}, 1, 100), st, cfg)
              == MoveReject::None, "valid small move -> None");
    CHECK(ValidateMovement(MakeCmd(1, st.pos, Position{too_fast * 1.2f, 0, 0, 0}, 1, 100), st, cfg)
              == MoveReject::TooFast, "over tolerance -> TooFast");
    CHECK(ValidateMovement(MakeCmd(1, st.pos, Position{teleport * 3.0f, 0, 0, 0}, 1, 100), st, cfg)
              == MoveReject::Teleport, "huge jump -> Teleport");
    // 世界边界：贴边小幅越界（位移 0.2m，远低于瞬移阈值）→ 钳制到边界后接受。
    {
        MovementState edge;
        edge.pos = Position{kWorld - 0.1f, 0, 0, 0};
        edge.max_speed = cfg.max_speed;
        CHECK(ValidateMovement(MakeCmd(1, edge.pos, Position{kWorld + 0.1f, 0, 0, 0}, 1, 100), edge, cfg)
                  == MoveReject::OutOfBounds, "near-edge tiny overshoot -> OutOfBounds (clamp)");
    }
    // 巨幅越界跳跃必须按瞬移**拒绝**：若先钳制边界，客户端上报 x=2e6 会被免费
    // 传送到边界 x=1e6（瞬移漏洞）。故 速度/瞬移 判定必须早于 边界 判定（§8 / §21）。
    CHECK(ValidateMovement(MakeCmd(1, st.pos, Position{kWorld * 2.0f, 0, 0, 0}, 1, 100), st, cfg)
              == MoveReject::Teleport, "huge out-of-bounds jump -> Teleport (no clamp-exploit)");
    st.move_flags = kMoveFlagStunned;
    CHECK(ValidateMovement(MakeCmd(1, st.pos, Position{too_fast * 0.5f, 0, 0, 0}, 1, 100), st, cfg)
              == MoveReject::NotMovable, "stunned -> NotMovable");
    st.move_flags = 0;
    st.last_client_seq = 10;
    CHECK(ValidateMovement(MakeCmd(1, st.pos, Position{too_fast * 0.5f, 0, 0, 0}, 10, 200), st, cfg)
              == MoveReject::RateLimited, "replay same seq -> RateLimited");
    CHECK(ValidateMovement(MakeCmd(1, st.pos, Position{too_fast * 0.5f, 0, 0, 0}, 5, 200), st, cfg)
              == MoveReject::RateLimited, "out-of-order seq -> RateLimited");
    st.last_client_seq = 0;
    st.last_client_timestamp_ms = 1000;
    CHECK(ValidateMovement(MakeCmd(1, st.pos, Position{too_fast * 0.5f, 0, 0, 0}, 1, 1010), st, cfg)
              == MoveReject::RateLimited, "too frequent -> RateLimited");
}

// ---------------------------------------------------------------------------
// 2. 积分精度：匀速 10 秒位移误差 < 1cm
// ---------------------------------------------------------------------------
void TestIntegrationPrecision() {
    TestHarness h;
    MovementSystem ms;
    const EntityId id = 1;
    (void)h.mgr.Create(mmo::game::EntityType::Player, kScene, Position{0, 0, 0, 0});
    MovementState st;
    st.pos = Position{0, 0, 0, 0};
    st.last_client_update = h.ctx.now;  // 避免外推超时停转
    (void)ms.Register(id, st);
    (void)ms.SetVelocity(id, Vec3{6.0f, 0.0f, 0.0f});  // 6 m/s 沿 +x

    constexpr int ticks = 200;          // 20Hz × 10s
    constexpr float dt = 0.05f;
    for (int i = 0; i < ticks; ++i) {
        h.ctx.tick_number = static_cast<std::uint64_t>(i + 1);  // 与 moved_tick(0) 不同 → 推进
        (void)ms.Integrate(h.ctx, dt);
    }
    auto s = ms.StateOf(id);
    CHECK(s.HasValue(), "state exists");
    const float expected = 6.0f * dt * ticks;  // 60.0m
    const float err = std::fabs(s.Value()->pos.x - expected);
    CHECK(err < 0.01f, "uniform 10s displacement error < 1cm");
    LineFmt("  [precision] x=%.6f expected=%.6f err=%.3e m\n", s.Value()->pos.x, expected, err);

    MovementState st2;
    st2.pos = Position{1, 2, 3, 0};
    (void)ms.Register(2, st2);
    (void)ms.Integrate(h.ctx, dt);  // 无速度 → 跳过
    auto s2 = ms.StateOf(2);
    CHECK(s2.HasValue() && std::fabs(s2.Value()->pos.x - 1.0f) < 1e-6f, "zero-velocity entity unmoved");
}

// ---------------------------------------------------------------------------
// 3. SetSpeed / SetVelocity / Stop
// ---------------------------------------------------------------------------
void TestSpeedAndStop() {
    MovementSystem ms;
    const EntityId id = 7;
    (void)ms.Register(id, MovementState{});

    CHECK(ms.SetSpeed(id, 10.0f).HasValue(), "SetSpeed ok");
    CHECK(ms.StateOf(id).Value()->max_speed == 10.0f, "max_speed updated");
    CHECK_CODE(ms.SetSpeed(id, -1.0f), ErrorCode::INVALID_ARGUMENT, "negative speed rejected");
    CHECK(ms.SetVelocity(id, Vec3{1, 2, 3}).HasValue(), "SetVelocity ok");
    CHECK(std::fabs(ms.StateOf(id).Value()->speed - std::sqrt(14.0f)) < 1e-5f, "speed scalar");
    CHECK_CODE(ms.SetVelocity(id, Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0}),
               ErrorCode::INVALID_ARGUMENT, "NaN velocity rejected");
    CHECK(ms.Stop(id).HasValue(), "Stop ok");
    CHECK(ms.StateOf(id).Value()->speed == 0.0f, "Stop zeroes speed");
    CHECK_CODE(ms.Stop(999), ErrorCode::NOT_FOUND, "Stop unknown -> NOT_FOUND");
}

// ---------------------------------------------------------------------------
// 4. client_seq 乱序 / 重放检测
// ---------------------------------------------------------------------------
void TestClientSeqReplay() {
    TestHarness h;
    MovementSystem ms;
    const EntityId id = 1;
    (void)h.mgr.Create(mmo::game::EntityType::Player, kScene, Position{0, 0, 0, 0});
    MovementState st;
    st.pos = Position{0, 0, 0, 0};
    st.last_client_seq = 10;
    st.last_client_timestamp_ms = 1000;
    (void)ms.Register(id, st);

    CHECK_CODE(ms.ApplyCommand(MakeCmd(id, Position{0, 0, 0, 0}, Position{0.1f, 0, 0, 0}, 10, 2000), h.ctx),
               ErrorCode::RATE_LIMITED, "replay same seq -> RateLimited");
    CHECK_CODE(ms.ApplyCommand(MakeCmd(id, Position{0, 0, 0, 0}, Position{0.1f, 0, 0, 0}, 5, 2000), h.ctx),
               ErrorCode::RATE_LIMITED, "out-of-order seq -> RateLimited");
    CHECK(ms.ApplyCommand(MakeCmd(id, Position{0, 0, 0, 0}, Position{0.1f, 0, 0, 0}, 11, 2000), h.ctx).HasValue(),
          "in-order seq accepted");
}

// ---------------------------------------------------------------------------
// 5. 拒绝原因计数 + 纠偏
// ---------------------------------------------------------------------------
void TestRejectStats() {
    TestHarness h;
    // 纠偏阈值调小（0.05m），使「钳制导致的偏差」必然触发纠偏计数；
    // 默认 0.5m 阈值下单次钳制偏差最大仅 ~0.555m，断言不稳。
    MovementConfig mcfg;
    mcfg.correction_threshold = 0.05f;
    MovementSystem ms(mcfg);
    const EntityId id = 1;
    (void)h.mgr.Create(mmo::game::EntityType::Player, kScene, Position{0, 0, 0, 0});
    MovementState st;
    st.pos = Position{0, 0, 0, 0};
    (void)ms.Register(id, st);
    ms.ResetStats();

    CHECK(ms.ApplyCommand(MakeCmd(id, Position{0, 0, 0, 0}, Position{0.1f, 0, 0, 0}, 1, 100), h.ctx).HasValue(),
          "normal move accepted");
    // 位移 0.5m：> 容差 0.345m（TooFast）但 < 瞬移 0.9m → 钳制后接受。
    CHECK(ms.ApplyCommand(MakeCmd(id, Position{0.1f, 0, 0, 0}, Position{0.6f, 0, 0, 0}, 2, 200), h.ctx).HasValue(),
          "overspeed clamped-accepted");
    CHECK_CODE(ms.ApplyCommand(MakeCmd(id, Position{0.1f, 0, 0, 0}, Position{100.0f, 0, 0, 0}, 3, 300), h.ctx),
               ErrorCode::INVALID_ARGUMENT, "teleport rejected");

    MovementStats s = ms.Stats();
    CHECK(s.rejected_by_reason[static_cast<std::size_t>(MoveReject::TooFast)] == 1, "TooFast counted");
    CHECK(s.rejected_by_reason[static_cast<std::size_t>(MoveReject::Teleport)] == 1, "Teleport counted");
    CHECK(s.corrections >= 1, "correction counted on clamp");
    CHECK(s.moved_count == 2, "moved_count (normal + clamped)");
}

// ---------------------------------------------------------------------------
// 6. 反作弊：10 倍速 / 瞬移 / 高频 全部拒绝且计数
// ---------------------------------------------------------------------------
void TestAntiCheat() {
    TestHarness h;
    MovementSystem ms;
    const EntityId id = 1;
    (void)h.mgr.Create(mmo::game::EntityType::Player, kScene, Position{0, 0, 0, 0});
    MovementState st;
    st.pos = Position{0, 0, 0, 0};
    (void)ms.Register(id, st);
    ms.ResetStats();

    const float step = ms.GetConfig().max_speed * ms.GetConfig().tick_dt;  // 0.3m
    // 10× 单 Tick 位移（> 3× 阈值）→ Teleport 拒绝 + 计数
    CHECK_CODE(ms.ApplyCommand(MakeCmd(id, Position{0, 0, 0, 0}, Position{step * 10.0f, 0, 0, 0}, 1, 100), h.ctx),
               ErrorCode::INVALID_ARGUMENT, "10x speed rejected");
    CHECK(ms.Stats().rejected_by_reason[static_cast<std::size_t>(MoveReject::Teleport)] >= 1,
          "10x speed teleport counted");

    // 高频：第二条命令时间戳间隔 < 1/max_cps（33ms）→ RateLimited
    ms.ResetStats();
    CHECK(ms.ApplyCommand(MakeCmd(id, Position{0, 0, 0, 0}, Position{step * 0.5f, 0, 0, 0}, 2, 1000), h.ctx).HasValue(),
          "first cmd accepted");
    CHECK_CODE(ms.ApplyCommand(MakeCmd(id, Position{step * 0.5f, 0, 0, 0}, Position{step * 0.6f, 0, 0, 0}, 3, 1010),
                               h.ctx),
               ErrorCode::RATE_LIMITED, "high-freq rejected");
}

// ---------------------------------------------------------------------------
// 7. 失败路径（§19）：NaN / 极端坐标 / 已销毁实体 / AOI 失败不回滚
// ---------------------------------------------------------------------------
void TestFailurePaths() {
    TestHarness h;
    MovementSystem ms;
    const EntityId id = 1;
    (void)h.mgr.Create(mmo::game::EntityType::Player, kScene, Position{0, 0, 0, 0});
    MovementState st;
    st.pos = Position{0, 0, 0, 0};
    (void)ms.Register(id, st);

    // NaN 坐标 → 拒绝并保持原位置
    Position nan_p;
    nan_p.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_CODE(ms.ApplyCommand(MakeCmd(id, Position{0, 0, 0, 0}, nan_p, 1, 100), h.ctx),
               ErrorCode::INVALID_ARGUMENT, "NaN rejected");
    CHECK(std::fabs(ms.StateOf(id).Value()->pos.x) < 1e-9f, "NaN keeps original pos");

    // 极端坐标（1e30）→ 拒绝，不污染状态
    CHECK_CODE(ms.ApplyCommand(MakeCmd(id, Position{0, 0, 0, 0}, Position{1.0e30f, 0, 0, 0}, 2, 200), h.ctx),
               ErrorCode::INVALID_ARGUMENT, "extreme coord rejected");
    CHECK(std::isfinite(ms.StateOf(id).Value()->pos.x), "extreme coord no overflow propagation");

    // 已销毁实体 → NOT_FOUND，不崩溃
    const auto cr = h.mgr.Create(mmo::game::EntityType::Player, kScene, Position{0, 0, 0, 0});
    const EntityId dead = cr.Value()->Id();
    (void)h.mgr.Destroy(dead);  // 逻辑死亡 + 世代 +1，Find(dead) 返回 nullptr
    CHECK_CODE(ms.ApplyCommand(MakeCmd(dead, Position{0, 0, 0, 0}, Position{0.1f, 0, 0, 0}, 1, 100), h.ctx),
               ErrorCode::NOT_FOUND, "destroyed entity -> NOT_FOUND");

    // AOI 失败不回滚：绑定 AOI 但实体未 Enter → aoi_->Move 返回 NOT_FOUND，位置仍更新
    aoi::AoiConfig acfg;
    // 变量名禁止叫 aoi：声明点位于初始化器之前，会遮蔽 aoi 命名空间导致 'aoi' is not a namespace。
    auto aoiInst = aoi::CreateDynamicGridAoi(acfg);
    MovementSystem ms2(MovementConfig{}, aoiInst.get());
    // EntityId = (index<<32) | generation，第 3 个实体是 8589934593 而非 3；
    // 必须取 Create 返回的真实 id，硬编码序号会导致 Find 失败（§7 冻结契约）。
    const auto cr3 = h.mgr.Create(mmo::game::EntityType::Player, kScene, Position{0, 0, 0, 0});
    const EntityId id3 = cr3.Value()->Id();
    MovementState st3;
    st3.pos = Position{0, 0, 0, 0};
    (void)ms2.Register(id3, st3);
    ms2.ResetStats();
    CHECK(ms2.ApplyCommand(MakeCmd(id3, Position{0, 0, 0, 0}, Position{0.1f, 0, 0, 0}, 1, 100), h.ctx).HasValue(),
          "move accepted despite AOI missing entity");
    CHECK(ms2.Stats().aoi_errors >= 1, "AOI failure counted");
    CHECK(std::fabs(ms2.StateOf(id3).Value()->pos.x - 0.1f) < 1e-6f, "position NOT rolled back on AOI failure");
}

// ---------------------------------------------------------------------------
// 8. 集成：1000 实体随机移动（含 10% 作弊），位置合法 + AOI 一致 + 纠偏 < 5%
// ---------------------------------------------------------------------------
void TestIntegration() {
    TestHarness h;
    aoi::AoiConfig acfg;
    auto aoiInst = aoi::CreateDynamicGridAoi(acfg);
    MovementSystem ms(MovementConfig{}, aoiInst.get());

    constexpr int N = 1000;
    constexpr int ticks = 300;
    std::mt19937 rng(20260831);
    std::uniform_real_distribution<float> coord(-kWorld * 0.01f, kWorld * 0.01f);  // ±1e4
    std::uniform_real_distribution<float> dir(-1.0f, 1.0f);
    std::uniform_real_distribution<float> cheat(0.0f, 1.0f);

    std::vector<EntityId> ids(N);
    std::vector<Position> pos(N);  // 权威位置镜像，供暴力对拍
    for (int i = 0; i < N; ++i) {
        Position p{coord(rng), coord(rng), coord(rng), 0};
        auto er = h.mgr.Create(mmo::game::EntityType::Player, kScene, p);
        ids[i] = er.Value()->Id();
        pos[i] = p;
        (void)aoiInst->Enter(ids[i], p);
        MovementState st;
        st.pos = p;
        (void)ms.Register(ids[i], st);
    }

    const float step = ms.GetConfig().max_speed * ms.GetConfig().tick_dt;  // 0.3m
    int cheat_count = 0;

    for (int t = 0; t < ticks; ++t) {
        h.ctx.tick_number = static_cast<std::uint64_t>(t);
        for (int i = 0; i < N; ++i) {
            const bool is_cheat = cheat(rng) < 0.10f;
            Position to;
            if (is_cheat) {
                to.x = pos[i].x + dir(rng) * step * 10.0f;  // 瞬移，应被拒
                to.y = pos[i].y + dir(rng) * step * 10.0f;
                to.z = pos[i].z + dir(rng) * step * 10.0f;
                ++cheat_count;
            } else {
                to.x = pos[i].x + dir(rng) * step * 0.9f;
                to.y = pos[i].y + dir(rng) * step * 0.9f;
                to.z = pos[i].z + dir(rng) * step * 0.9f;
            }
            auto r = ms.ApplyCommand(
                MakeCmd(ids[i], pos[i], to, static_cast<std::uint32_t>(t + 1),
                        static_cast<std::int64_t>((t + 1) * 40)), h.ctx);
            if (r.HasValue()) pos[i] = ms.StateOf(ids[i]).Value()->pos;  // 接受才更新镜像
        }
        (void)ms.Integrate(h.ctx, ms.GetConfig().tick_dt);  // 命令驱动的实体已跳过
        for (int i = 0; i < N; ++i) pos[i] = ms.StateOf(ids[i]).Value()->pos;

        if (t % 50 == 0) {  // AOI 一致性对拍（§17）
            const int sample = static_cast<int>(rng() % N);
            std::vector<EntityId> vis;
            (void)aoiInst->QueryVisible(ids[sample], vis);
            std::sort(vis.begin(), vis.end());
            std::vector<EntityId> ref;
            for (int j = 0; j < N; ++j) {
                if (j == sample) continue;
                const float dx = pos[sample].x - pos[j].x;
                const float dy = pos[sample].y - pos[j].y;
                const float dz = pos[sample].z - pos[j].z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) <= acfg.view_radius) ref.push_back(ids[j]);
            }
            std::sort(ref.begin(), ref.end());
            CHECK(vis == ref, "AOI visible set matches brute force");
        }
    }

    for (int i = 0; i < N; ++i) {  // 位置合法性
        const Position& p = pos[i];
        CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "finite pos");
        CHECK(std::fabs(p.x) <= kWorld && std::fabs(p.y) <= kWorld && std::fabs(p.z) <= kWorld,
              "in-bounds pos");
    }

    MovementStats s = ms.Stats();
    const std::size_t total_ops = static_cast<std::size_t>(N) * ticks;
    CHECK(s.corrections <= total_ops / 20u, "corrections < 5% of operations");
    CHECK(s.rejected_by_reason[static_cast<std::size_t>(MoveReject::Teleport)] > 0, "cheat detected");

    // 动量 / 外推：给实体 0 设速度，新 Tick（≠ 上次命令 Tick）下 Integrate 应推进
    h.ctx.tick_number = static_cast<std::uint64_t>(ticks);
    (void)ms.SetVelocity(ids[0], Vec3{6.0f, 0.0f, 0.0f});
    Position before = ms.StateOf(ids[0]).Value()->pos;
    (void)ms.Integrate(h.ctx, ms.GetConfig().tick_dt);
    Position after = ms.StateOf(ids[0]).Value()->pos;
    CHECK(std::fabs((after.x - before.x) - 6.0f * ms.GetConfig().tick_dt) < 1e-3f, "momentum advances");

    LineFmt("  [integration] N=%d ticks=%d cheat=%d rejected[Teleport]=%zu corrections=%zu\n",
            N, ticks, cheat_count,
            s.rejected_by_reason[static_cast<std::size_t>(MoveReject::Teleport)], s.corrections);
}

}  // namespace

int main() {
    Line("== TASK-015 movement_test ==\n");
    TestValidateRules();
    TestIntegrationPrecision();
    TestSpeedAndStop();
    TestClientSeqReplay();
    TestRejectStats();
    TestAntiCheat();
    TestFailurePaths();
    TestIntegration();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
