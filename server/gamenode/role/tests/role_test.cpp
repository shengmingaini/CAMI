// server/gamenode/role/tests/role_test.cpp — TASK-016 §16 单元 / §17 集成 / §19 Failure
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
// ctest 标签：Role（验收脚本 run_ctest 'Role'）。

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/config/config_manager.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_events.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"

namespace {

// 注：本文件位于全局匿名命名空间，mmo::core / mmo::game 均为 mmo 的嵌套命名空间，
// 非限定名 core:: / role:: 不会自动解析，必须显式引入（禁止裸写 using namespace mmo;）。
namespace core = mmo::core;
using namespace mmo::game;             // EntityManager / SceneContext / EntityId / PlayerId
using namespace mmo::game::role;       // RoleSystem / Character / AttributeSet ...

using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
using mmo::core::ErrorCode;

int g_fails = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                    \
            ++g_fails;                                                          \
        }                                                                       \
    } while (0)

#define CHECK_CODE(result, expected, msg)                                        \
    do {                                                                         \
        const auto& r_ = (result);                                               \
        if (r_.HasValue()) {                                                     \
            ErrorFmt("FAIL: %s (line %d): expected error, got ok\n", msg, __LINE__); \
            ++g_fails;                                                           \
        } else if (r_.Err().Code() != (expected)) {                              \
            ErrorFmt("FAIL: %s (line %d): code mismatch\n", msg, __LINE__);       \
            ++g_fails;                                                           \
        }                                                                        \
    } while (0)

constexpr std::uint64_t kScene = 7;
constexpr const char* kExpCurvePath = "config/gameplay/exp_curve.json";

/// 最小 SceneContext（bus / scheduler / arena 均为测试本地实例）。
struct TestHarness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena{256 * 1024};
    EntityManager mgr{&bus};
    SceneContext ctx;

    TestHarness()
        : ctx(kScene, SceneType::World, 1, core::MonotonicClock::Point(), 0, mgr, bus,
              scheduler, arena) {}
};

/// 事件计数器（订阅后需 bus.Drain() 才派发）。
struct EventCounters {
    std::size_t attributes_changed{0};
    std::size_t level_up{0};
    std::size_t hp_changed{0};
    std::size_t mp_changed{0};
    std::size_t died{0};
    std::vector<std::uint32_t> level_ups;  // 依次升级到的等级

    void Bind(core::EventBus& bus) noexcept {
        (void)bus.Subscribe<AttributesChanged>(
            [this](const AttributesChanged& e) { (void)e; ++attributes_changed; });
        (void)bus.Subscribe<LevelUp>([this](const LevelUp& e) {
            ++level_up;
            level_ups.push_back(e.new_level);
        });
        (void)bus.Subscribe<HpChanged>([this](const HpChanged& e) { (void)e; ++hp_changed; });
        (void)bus.Subscribe<MpChanged>([this](const MpChanged& e) { (void)e; ++mp_changed; });
        (void)bus.Subscribe<CharacterDied>([this](const CharacterDied& e) { (void)e; ++died; });
    }
};

/// 造一条只含合法曲线的 ExpCurve（与线上配置文件解耦，纯单元用）。
ExpCurve MakeTestCurve() {
    ExpCurveConfig cfg;
    cfg.max_level = 60;
    cfg.base = 100.0;
    cfg.exponent = 1.5;
    auto c = ExpCurve::FromConfig(cfg);
    CHECK(c.HasValue(), "ExpCurve::FromConfig should accept valid config");
    return std::move(c).Value();
}

// ---------------------------------------------------------------------------
// §16 · 属性三层模型
// ---------------------------------------------------------------------------

void TestAttributeThreeLayer() {
    Line("[unit] attribute three-layer model\n");

    AttributeSet a;
    a.base[static_cast<std::size_t>(AttrType::Strength)] = 10;
    a.from_equipment[static_cast<std::size_t>(AttrType::Strength)] = 5;
    a.from_buff[static_cast<std::size_t>(AttrType::Strength)] = 3;
    a.Recompute();
    CHECK(a.Total(AttrType::Strength) == 18, "Strength final = 10 + 5 + 3");

    // 任一层变更后必须 Recompute，Final 才更新（禁止各系统直接改 Final）。
    a.from_buff[static_cast<std::size_t>(AttrType::Strength)] = 10;
    CHECK(a.Total(AttrType::Strength) == 18, "Final unchanged before Recompute");
    a.Recompute();
    CHECK(a.Total(AttrType::Strength) == 25, "Final refreshed after Recompute");

    // 钳制：主属性上限 9999（配置化上限，见 attribute.cpp DefaultLimits）。
    AttributeSet b;
    b.base[static_cast<std::size_t>(AttrType::Agility)] = 100000;
    b.Recompute();
    CHECK(b.Total(AttrType::Agility) == 9999, "primary attr clamped to max");

    // 负值钳制。
    AttributeSet c;
    c.base[static_cast<std::size_t>(AttrType::Intellect)] = 50;
    c.from_buff[static_cast<std::size_t>(AttrType::Intellect)] = -500;
    c.Recompute();
    CHECK(c.Total(AttrType::Intellect) == 0, "primary attr clamped to min 0");

    // 派生属性：MaxHp = hp_per_stamina * Stamina + hp_base（系数配置化，测试从同一来源取）。
    const AttrFormula& f = AttrFormulaFor();
    AttributeSet d;
    d.base[static_cast<std::size_t>(AttrType::Stamina)] = 20;
    d.Recompute();
    CHECK(d.Total(AttrType::MaxHp) == f.hp_per_stamina * 20 + f.hp_base,
          "MaxHp derived from Stamina by formula");

    // 派生属性的三层可作为「固定加成」叠加（装备/Buff 的 +MaxHp）。
    d.from_equipment[static_cast<std::size_t>(AttrType::MaxHp)] = 500;
    d.Recompute();
    CHECK(d.Total(AttrType::MaxHp) == f.hp_per_stamina * 20 + f.hp_base + 500,
          "derived attr accepts flat bonus from equipment layer");
}

/// §15.1「配置化上限」：钳制区间与派生公式是**进程级配置**，不是 Recompute 里的魔数。
/// 本用例证明 SetAttrLimit / SetAttrFormula 能改变 Recompute 的结果（即可配置、可覆盖），
/// 并在结束时复位，避免污染后续用例。
void TestAttrLimitsAreConfigurable() {
    Line("[unit] attr limits / formula are configurable\n");

    AttributeSet a;
    a.base[static_cast<std::size_t>(AttrType::Strength)] = 5000;
    a.Recompute();
    CHECK(a.Total(AttrType::Strength) == 5000, "5000 within default limit 9999");

    SetAttrLimit(AttrType::Strength, AttrLimit{0, 100});
    a.Recompute();
    CHECK(a.Total(AttrType::Strength) == 100, "recomputed against overridden limit");

    // 派生公式同样可覆盖。
    AttributeSet b;
    b.base[static_cast<std::size_t>(AttrType::Stamina)] = 10;
    b.Recompute();
    const std::int64_t before = b.Total(AttrType::MaxHp);
    AttrFormula f = AttrFormulaFor();
    const std::int64_t saved_per = f.hp_per_stamina;
    f.hp_per_stamina = saved_per * 2;
    SetAttrFormula(f);
    b.Recompute();
    CHECK(b.Total(AttrType::MaxHp) == before + saved_per * 10, "formula override takes effect");

    ResetAttrLimits();
    ResetAttrFormula();
    a.Recompute();
    CHECK(a.Total(AttrType::Strength) == 5000, "limits restored after reset");
}

// ---------------------------------------------------------------------------
// §16 · 经验曲线配置化与边界
// ---------------------------------------------------------------------------

void TestExpCurveConfig() {
    Line("[unit] exp curve config-driven\n");

    // §19 配置缺失 → 报错，禁止默认值静默启动。
    core::ConfigManager::ResetForTest();
    auto missing = ExpCurve::LoadFromFile("config/gameplay/__no_such_file__.json");
    CHECK_CODE(missing, ErrorCode::NOT_FOUND, "missing exp curve config must fail");

    // 真实配置文件（工作目录 = 仓库根）。
    core::ConfigManager::ResetForTest();
    auto loaded = ExpCurve::LoadFromFile(kExpCurvePath);
    CHECK(loaded.HasValue(), "exp_curve.json must load");
    if (!loaded.HasValue()) return;
    ExpCurve curve = std::move(loaded).Value();

    CHECK(curve.MaxLevel() >= 2, "max level from config");
    CHECK(curve.ExpToNext(1) > 0, "level 1 requires positive exp");

    // 单调性：等级越高，升级所需经验越多（base>0, exponent>0）。
    bool monotonic = true;
    for (std::uint32_t lv = 1; lv + 1 < curve.MaxLevel(); ++lv) {
        if (curve.ExpToNext(lv + 1) <= curve.ExpToNext(lv)) monotonic = false;
    }
    CHECK(monotonic, "exp_to_next strictly increasing with level");

    // 边界：0 级按 1 级算（防御），满级返回 0。
    CHECK(curve.ExpToNext(0) == curve.ExpToNext(1), "level 0 falls back to level 1");
    CHECK(curve.ExpToNext(curve.MaxLevel()) == 0, "max level requires no more exp");

    // 非法配置 → INVALID_ARGUMENT（禁止产出一条"看起来能用"的坏曲线）。
    ExpCurveConfig bad_zero_base{};
    bad_zero_base.max_level = 60;
    bad_zero_base.base = 0.0;
    bad_zero_base.exponent = 1.5;
    CHECK_CODE(ExpCurve::FromConfig(bad_zero_base), ErrorCode::INVALID_ARGUMENT,
               "zero base rejected");

    ExpCurveConfig bad_level{};
    bad_level.max_level = 0;
    bad_level.base = 100.0;
    bad_level.exponent = 1.5;
    CHECK_CODE(ExpCurve::FromConfig(bad_level), ErrorCode::INVALID_ARGUMENT,
               "zero max level rejected");
}

// ---------------------------------------------------------------------------
// §16 · LoadOrCreate / AttachToScene / 版本递增
// ---------------------------------------------------------------------------

void TestLoadOrCreateAndAttach() {
    Line("[unit] LoadOrCreate / AttachToScene / version\n");

    TestHarness h;
    InMemoryPersistenceAdapter sink;
    RoleSystem sys(sink, MakeTestCurve());
    sys.BindEventBus(h.bus);

    const PlayerId pid = 1001;
    const CharacterId cid = 5001;

    auto c = sys.LoadOrCreate(pid, cid, h.ctx);
    CHECK(c.HasValue(), "LoadOrCreate ok");
    if (!c.HasValue()) return;
    Character* ch = c.Value();

    CHECK(ch->id == cid && ch->owner == pid, "character ids bound");
    CHECK(ch->level == 1, "new character starts at level 1");
    CHECK(ch->exp == 0, "new character starts with 0 exp");
    CHECK(ch->hp == ch->MaxHp(), "new character spawns at full hp");
    CHECK(ch->mp == ch->MaxMp(), "new character spawns at full mp");
    CHECK(ch->version >= 1, "version starts >= 1");

    // 幂等：重复 LoadOrCreate 返回同一对象，不新建。
    auto again = sys.LoadOrCreate(pid, cid, h.ctx);
    CHECK(again.HasValue() && again.Value() == ch, "LoadOrCreate is idempotent");
    CHECK(sys.CharacterCount() == 1, "no duplicate character created");

    // Find / FindByPlayer。
    CHECK(sys.Find(cid) == ch, "Find by CharacterId");
    CHECK(sys.FindByPlayer(pid) == ch, "FindByPlayer");
    CHECK(sys.Find(999999) == nullptr, "Find unknown -> nullptr");

    // AttachToScene：只记 id，不持有 Scene。
    const std::uint32_t before = ch->version;
    CHECK(sys.AttachToScene(cid, static_cast<EntityId>(0x1234), kScene).HasValue(),
          "AttachToScene ok");
    CHECK(ch->avatar == static_cast<EntityId>(0x1234), "avatar id stored");
    CHECK(ch->scene == kScene, "scene id stored");
    CHECK(ch->version == before + 1, "AttachToScene bumps version");

    // 非法 id。
    CHECK_CODE(sys.LoadOrCreate(pid, kInvalidCharacterId, h.ctx), ErrorCode::INVALID_ARGUMENT,
               "invalid character id rejected");
}

// ---------------------------------------------------------------------------
// §16 · HP/MP 钳制与边界（§15 步骤 6）
// ---------------------------------------------------------------------------

void TestVitalClamp() {
    Line("[unit] HP/MP clamping\n");

    TestHarness h;
    InMemoryPersistenceAdapter sink;
    RoleSystem sys(sink, MakeTestCurve());
    sys.BindEventBus(h.bus);
    EventCounters ev;
    ev.Bind(h.bus);

    Character* ch = sys.LoadOrCreate(1, 11, h.ctx).Value();
    const std::int64_t max_hp = ch->MaxHp();
    const std::int64_t max_mp = ch->MaxMp();
    CHECK(max_hp > 0 && max_mp > 0, "positive max hp/mp");

    // 扣到负数 → 钳制到 0，绝不出现负值。
    CHECK(sys.ModifyHp(11, -(max_hp + 1000), core::NewTraceID()).HasValue(), "ModifyHp ok");
    CHECK(ch->hp == 0, "hp clamped to 0, never negative");

    // 超上限 → 钳制到 MaxHp。
    CHECK(sys.ModifyHp(11, max_hp + 1000, core::NewTraceID()).HasValue(), "heal ok");
    CHECK(ch->hp == max_hp, "hp clamped to MaxHp");

    // MP 同理。
    CHECK(sys.ModifyMp(11, -(max_mp + 1), core::NewTraceID()).HasValue(), "ModifyMp ok");
    CHECK(ch->mp == 0, "mp clamped to 0");
    CHECK(sys.ModifyMp(11, max_mp + 1, core::NewTraceID()).HasValue(), "Mp restore ok");
    CHECK(ch->mp == max_mp, "mp clamped to MaxMp");

    // 溢出防御：int64 极值不得让钳制语义反转。
    CHECK(sys.ModifyHp(11, std::numeric_limits<std::int64_t>::max(), core::NewTraceID())
              .HasValue(),
          "ModifyHp with int64 max must not overflow");
    CHECK(ch->hp == max_hp, "hp still clamped after int64 max delta");

    (void)h.bus.Drain();
    CHECK(ev.hp_changed >= 3, "HpChanged events published");
    CHECK(ev.mp_changed >= 2, "MpChanged events published");
    CHECK(ev.died == 1, "exactly one death event for the clamp-to-zero above");
}

// ---------------------------------------------------------------------------
// §16 · 死亡状态与事件（§15 步骤 7 / §15.7）
// ---------------------------------------------------------------------------

void TestDeathEvent() {
    Line("[unit] death state and event\n");

    TestHarness h;
    InMemoryPersistenceAdapter sink;
    RoleSystem sys(sink, MakeTestCurve());
    sys.BindEventBus(h.bus);
    EventCounters ev;
    ev.Bind(h.bus);

    Character* ch = sys.LoadOrCreate(2, 22, h.ctx).Value();

    // 未死亡。
    CHECK(!IsDead(ch->flags), "alive at spawn");

    // 致命一击 → 死亡标记 + 一次事件。
    CHECK(sys.ModifyHp(22, -(ch->MaxHp() + 1), core::NewTraceID()).HasValue(), "lethal hit");
    CHECK(ch->hp == 0, "hp zero");
    CHECK(IsDead(ch->flags), "dead flag set");

    // 重复致死 → 不重复发事件（§15.7）。
    for (int i = 0; i < 5; ++i) {
        CHECK(sys.ModifyHp(22, -100, core::NewTraceID()).HasValue(), "hit while dead");
    }
    (void)h.bus.Drain();
    CHECK(ev.died == 1, "CharacterDied published exactly once");
    CHECK(sys.Stats().deaths == 1, "death counter == 1");

    // 复活（回血）→ 清死亡标记，可再次触发死亡事件。
    CHECK(sys.ModifyHp(22, ch->MaxHp(), core::NewTraceID()).HasValue(), "resurrect");
    CHECK(!IsDead(ch->flags), "dead flag cleared on heal");
    CHECK(sys.ModifyHp(22, -(ch->MaxHp() + 1), core::NewTraceID()).HasValue(), "die again");
    (void)h.bus.Drain();
    CHECK(ev.died == 2, "second death published after resurrect");
}

// ---------------------------------------------------------------------------
// §16 · AddExp 跨级与版本递增
// ---------------------------------------------------------------------------

void TestAddExp() {
    Line("[unit] AddExp level-ups\n");

    TestHarness h;
    InMemoryPersistenceAdapter sink;
    RoleSystem sys(sink, MakeTestCurve());
    sys.BindEventBus(h.bus);
    EventCounters ev;
    ev.Bind(h.bus);

    Character* ch = sys.LoadOrCreate(3, 33, h.ctx).Value();
    const std::int64_t str0 = ch->attrs.Total(AttrType::Strength);
    const std::int64_t maxhp0 = ch->MaxHp();

    // 单级：给够 1 级的经验。
    const std::uint64_t need1 = sys.Curve().ExpToNext(1);
    auto lv = sys.AddExp(33, need1, core::NewTraceID());
    CHECK(lv.HasValue(), "AddExp ok");
    CHECK(lv.Value() == 2, "one level up to 2");
    CHECK(ch->exp == 0, "exp reset after level up");
    CHECK(ch->attrs.Total(AttrType::Strength) == str0 + sys.Defaults().primary_per_level,
          "primary attr grows per level");
    CHECK(ch->MaxHp() > maxhp0, "MaxHp grows with Stamina");

    // 跨多级：一次性给足 2 → 10 级的经验，应连续升级且每级各发一次 LevelUp。
    std::uint64_t bulk = 0;
    for (std::uint32_t l = 2; l <= 9; ++l) bulk += sys.Curve().ExpToNext(l);
    auto lv2 = sys.AddExp(33, bulk, core::NewTraceID());
    CHECK(lv2.HasValue() && lv2.Value() == 10, "bulk exp levels up to 10 continuously");

    (void)h.bus.Drain();
    CHECK(ev.level_up == 9, "one LevelUp event per level gained");
    CHECK(ev.level_ups.size() == 9, "level up sequence recorded");
    bool ascending = true;
    for (std::size_t i = 1; i < ev.level_ups.size(); ++i) {
        if (ev.level_ups[i] != ev.level_ups[i - 1] + 1) ascending = false;
    }
    CHECK(ascending, "LevelUp events in ascending level order");

    // 满级：经验清零，不再升级（ExpToNext == 0）。
    const std::uint32_t max_level = sys.Curve().MaxLevel();
    std::uint64_t to_max = std::numeric_limits<std::uint64_t>::max() / 4;
    (void)sys.AddExp(33, to_max, core::NewTraceID());
    CHECK(ch->level == max_level, "capped at max level");
    CHECK(ch->exp == 0, "exp cleared at max level");
    CHECK(sys.Curve().ExpToNext(ch->level) == 0, "no exp needed at max level");

    // 版本递增：每次数据变更 +1。
    const std::uint32_t v0 = ch->version;
    CHECK(sys.ModifyHp(33, -1, core::NewTraceID()).HasValue(), "ModifyHp for version bump");
    CHECK(ch->version > v0, "version increments on data change");
}

// ---------------------------------------------------------------------------
// §19 · 失败路径
// ---------------------------------------------------------------------------

void TestFailurePaths() {
    Line("[failure] persistence / overflow / not-found\n");

    TestHarness h;
    InMemoryPersistenceAdapter sink;
    RoleSystem sys(sink, MakeTestCurve());
    sys.BindEventBus(h.bus);

    Character* ch = sys.LoadOrCreate(4, 44, h.ctx).Value();

    // 1) 存档失败（DataService 不可用）→ 进重试队列，不清 dirty。
    sink.InjectFailures(2);
    CHECK_CODE(sys.Save(44), ErrorCode::BUSY, "save enqueue failure surfaces error");
    CHECK(sys.RetryQueueSize() == 1, "failed save enters retry queue");
    CHECK((ch->flags & kCharFlagDirty) != 0u, "dirty flag kept after failed save");
    CHECK(sys.Stats().save_failed == 1, "save_failed counted");
    CHECK(sink.PendingCount() == 0, "nothing enqueued while backend unavailable");

    // 重复失败不重复入队（队列按角色去重）。
    CHECK_CODE(sys.Save(44), ErrorCode::BUSY, "second failure");
    CHECK(sys.RetryQueueSize() == 1, "retry queue deduplicated by character");

    // 后端恢复 → FlushRetries 重投成功，队列清空、dirty 清除。
    CHECK(sys.FlushRetries() == 1, "retry flush succeeds once backend recovers");
    CHECK(sys.RetryQueueSize() == 0, "retry queue drained");
    CHECK((ch->flags & kCharFlagDirty) == 0u, "dirty cleared after successful save");
    CHECK(sink.PendingCount() == 1, "save reached the sink after retry");

    // 2) 经验溢出 → 报错（禁止静默回绕）。
    ch->exp = std::numeric_limits<std::uint64_t>::max() - 10;
    CHECK_CODE(sys.AddExp(44, 1000, core::NewTraceID()), ErrorCode::INVALID_ARGUMENT,
               "exp overflow rejected");
    CHECK(ch->exp == std::numeric_limits<std::uint64_t>::max(), "exp pinned at uint64 max");

    // 3) 角色不存在 → NOT_FOUND。
    CHECK_CODE(sys.ModifyHp(999999, -1, core::NewTraceID()), ErrorCode::NOT_FOUND,
               "ModifyHp on missing character");
    CHECK_CODE(sys.ModifyMp(999999, -1, core::NewTraceID()), ErrorCode::NOT_FOUND,
               "ModifyMp on missing character");
    CHECK_CODE(sys.AddExp(999999, 1, core::NewTraceID()), ErrorCode::NOT_FOUND,
               "AddExp on missing character");
    CHECK_CODE(sys.RecomputeAttributes(999999), ErrorCode::NOT_FOUND,
               "RecomputeAttributes on missing character");
    CHECK_CODE(sys.Save(999999), ErrorCode::NOT_FOUND, "Save on missing character");
    CHECK_CODE(sys.AttachToScene(999999, 1, kScene), ErrorCode::NOT_FOUND,
               "AttachToScene on missing character");
}

// ---------------------------------------------------------------------------
// §16 · RecomputeAttributes（三层来源变更后重算 + HP/MP 重钳制）
// ---------------------------------------------------------------------------

void TestRecomputeAttributes() {
    Line("[unit] RecomputeAttributes re-clamps vitals\n");

    TestHarness h;
    InMemoryPersistenceAdapter sink;
    RoleSystem sys(sink, MakeTestCurve());

    Character* ch = sys.LoadOrCreate(5, 55, h.ctx).Value();
    const std::int64_t hp0 = ch->hp;

    // 模拟 TASK-023 Buff 系统写入 buff 层后请求重算。
    ch->attrs.from_buff[static_cast<std::size_t>(AttrType::Stamina)] = 100;
    CHECK(sys.RecomputeAttributes(55).HasValue(), "RecomputeAttributes ok");
    CHECK(ch->MaxHp() > hp0, "MaxHp increased after buff");
    CHECK(ch->hp == hp0, "current hp untouched when upper bound grows");

    // 上限下调 → 当前 HP 被重新钳制（§21 禁止超过 Max）。
    ch->attrs.from_buff[static_cast<std::size_t>(AttrType::Stamina)] = -1000;
    CHECK(sys.RecomputeAttributes(55).HasValue(), "RecomputeAttributes ok (shrink)");
    CHECK(ch->hp <= ch->MaxHp(), "hp re-clamped below new MaxHp");
    CHECK(ch->mp <= ch->MaxMp(), "mp re-clamped below new MaxMp");
    CHECK(ch->hp >= 0 && ch->mp >= 0, "vitals never negative after recompute");
}

// ---------------------------------------------------------------------------
// §17 · 集成：1000 角色 1000 Tick（随机升级 / 掉血 + 每 100 Tick 批量存档）
// ---------------------------------------------------------------------------

void TestIntegration1000() {
    Line("[integration] 1000 characters x 1000 ticks\n");

    TestHarness h;
    InMemoryPersistenceAdapter sink;
    RoleSystem sys(sink, MakeTestCurve());
    sys.BindEventBus(h.bus);
    sys.Reserve(1000);

    constexpr std::size_t kChars = 1000;
    constexpr std::size_t kTicks = 1000;
    constexpr std::size_t kOpsPerTick = 100;

    std::vector<CharacterId> ids;
    ids.reserve(kChars);
    for (std::size_t i = 0; i < kChars; ++i) {
        const CharacterId cid = 10000 + i;
        auto c = sys.LoadOrCreate(static_cast<PlayerId>(i + 1), cid, h.ctx);
        if (!c.HasValue()) {
            CHECK(false, "bulk LoadOrCreate failed");
            return;
        }
        ids.push_back(cid);
    }
    CHECK(sys.CharacterCount() == kChars, "1000 characters loaded");

    std::mt19937 rng(0x5EED1234u);
    std::uniform_int_distribution<std::size_t> pick(0, kChars - 1);

    std::vector<double> tick_us;
    tick_us.reserve(kTicks);
    std::vector<double> save_tick_us;  // 含批量存档的 Tick（§17「存档不阻塞 Tick」观测项）
    save_tick_us.reserve(kTicks / 100 + 1);
    std::size_t save_batches = 0;

    for (std::size_t t = 0; t < kTicks; ++t) {
        const core::SteadyNs t0 = core::MonotonicClock::Now();
        for (std::size_t k = 0; k < kOpsPerTick; ++k) {
            const CharacterId cid = ids[pick(rng)];
            (void)sys.AddExp(cid, 1, core::NewTraceID());
            (void)sys.ModifyHp(cid, -1, core::NewTraceID());
        }
        bool saved_this_tick = false;
        if ((t + 1) % 100 == 0) {
            for (CharacterId cid : ids) (void)sys.Save(cid);
            sink.Drain();  // 模拟后台线程已消费
            ++save_batches;
            saved_this_tick = true;
        }
        const core::SteadyNs t1 = core::MonotonicClock::Now();
        const double us = static_cast<double>(t1 - t0) / 1000.0;
        tick_us.push_back(us);
        if (saved_this_tick) save_tick_us.push_back(us);
    }

    // Tick P99（排序只发生在测试侧，不进热路径）。
    std::vector<double> sorted = tick_us;
    std::sort(sorted.begin(), sorted.end());
    const double p50 = sorted[sorted.size() / 2];
    const double p99 = sorted[static_cast<std::size_t>(sorted.size() * 0.99)];
    std::sort(save_tick_us.begin(), save_tick_us.end());
    const double save_tick_worst = save_tick_us.empty() ? 0.0 : save_tick_us.back();
    LineFmt("  tick_us p50=%.1f p99=%.1f | save_tick_us worst=%.1f (batches=%zu)\n", p50, p99,
            save_tick_worst, save_batches);

    CHECK(save_batches == 10, "10 batch saves across 1000 ticks");
    CHECK(p99 < 20000.0, "plain tick p99 stays far below 20ms");
    // §17：存档只入队不等待落盘，含 1000 次 Save 的 Tick 也必须远低于 50ms 帧预算（§19/§22）。
    CHECK(save_tick_worst < 20000.0, "tick containing a 1000-character batch save stays < 20ms");

    // 数据一致性：全量重算后属性不变（逐步变更 == 一次性重算）。
    bool consistent = true;
    for (CharacterId cid : ids) {
        Character* c = sys.Find(cid);
        if (c == nullptr) { consistent = false; break; }
        AttributeSet copy = c->attrs;
        c->attrs.Recompute();
        for (std::size_t i = 0; i < kAttrCount; ++i) {
            if (copy.Total(static_cast<AttrType>(i)) != c->attrs.Total(static_cast<AttrType>(i))) {
                consistent = false;
            }
        }
        if (c->hp < 0 || c->hp > c->MaxHp()) consistent = false;
        if (c->mp < 0 || c->mp > c->MaxMp()) consistent = false;
    }
    CHECK(consistent, "attributes and vitals consistent after 1000 ticks");
    CHECK(sys.CharacterCount() == kChars, "no character lost during the run");
    LineFmt("  level_ups=%zu deaths=%zu save_enqueued=%zu\n", sys.Stats().level_ups,
            sys.Stats().deaths, sys.Stats().save_enqueued);
}

}  // namespace

int main() {
    TestAttributeThreeLayer();
    TestAttrLimitsAreConfigurable();
    TestExpCurveConfig();
    TestLoadOrCreateAndAttach();
    TestVitalClamp();
    TestDeathEvent();
    TestAddExp();
    TestRecomputeAttributes();
    TestFailurePaths();
    TestIntegration1000();

    if (g_fails == 0) {
        Line("role_test: ALL PASS\n");
        return 0;
    }
    ErrorFmt("role_test: %d FAILURE(S)\n", g_fails);
    return 1;
}
