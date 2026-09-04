// server/gamenode/combat/tests/buff_test.cpp — TASK-023 §16 单元 / §17 集成 / §19 Failure
//
// 覆盖：
//   §16 单元：三种堆叠规则（None/Refresh/Independent）、到期清理、周期结算（DOT/HOT）、
//             先加后乘（Buff 只写 from_buff 层）、驱散（含不可驱散）、控制标记、护盾；
//   §19 失败：未知 buff_id → NOT_FOUND；槽位上限 → BUSY；驱散不可驱散 Buff → INVALID_ARGUMENT；
//   §21 配置化：LoadFromConfig 从 config/gameplay/buffs/buffs.json 加载；
//   §24 红线：src/buff 无 mysql/redis/grpc/kafka/ifstream/std::thread。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/combat/buff/buff_events.h"
#include "mmo/game/combat/buff/buff_system.h"
#include "mmo/game/combat/damage/damage.h"
#include "mmo/game/combat/damage/damage_formula.h"
#include "mmo/game/combat/damage/damage_system.h"
#include "mmo/game/combat/skill/combat_events.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::combat;
using namespace mmo::game::combat::buff;
using namespace mmo::game::role;

using core::test::ErrorFmt;
using core::test::LineFmt;

constexpr SceneId kScene = 1;
constexpr NodeId kOwner = 1;
constexpr const char* kFormulaPath = "config/gameplay/combat/formula.json";
constexpr const char* kBuffConfig = "config/gameplay/buffs/buffs.json";

int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                        \
    } while (0)

class FixedArena final : public core::Arena {
public:
    explicit FixedArena(std::size_t bytes) : core::Arena(bytes) {}
};

ExpCurve MakeCurve() {
    auto c = ExpCurve::FromConfig(ExpCurveConfig{60, 100.0, 1.0});
    if (!c.HasValue()) return ExpCurve::FromConfig(ExpCurveConfig{1, 1.0, 1.0}).Value();
    return c.Value();
}

void ForceAttr(Character* c, AttrType t, std::int64_t target) {
    c->attrs.Recompute();
    const std::int64_t cur = c->attrs.Total(t);
    if (cur != target) {
        c->attrs.base[static_cast<std::size_t>(t)] += (target - cur);
        c->attrs.Recompute();
    }
}

DamageFormula LoadedFormula() {
    auto f = DamageFormula::LoadFromFile(kFormulaPath);
    if (!f.HasValue()) {
        ErrorFmt("FATAL: cannot load %s\n", kFormulaPath);
        ++g_fail;
        return DamageFormula{};
    }
    return f.Value();
}

struct Harness {
    InMemoryPersistenceAdapter sink;
    core::EventBus bus;
    RoleSystem roles{sink, MakeCurve()};
    EntityManager mgr;
    FixedArena arena{1u << 20};
    core::Scheduler sched;
    DamageSystem dmg;
    BuffRegistry registry;
    BuffSystem buffs;
    std::uint64_t tick{0};
    std::size_t buff_applied{0};
    std::size_t buff_removed{0};
    std::size_t buff_expired{0};
    std::size_t buff_dispelled{0};

    explicit Harness(const DamageFormula& f)
        : dmg(f, roles, mgr, kScene, &bus), buffs(registry, roles, &dmg, &bus) {
        // 预载 Buff 注册表（所有 Apply 测试依赖）。
        auto lr = registry.LoadFromConfig(kBuffConfig);
        if (!lr.HasValue()) {
            ErrorFmt("FATAL: cannot load buff config %s\n", kBuffConfig);
            ++g_fail;
        }
        roles.BindEventBus(bus);
        dmg.SetShieldSource(&buffs);  // BuffSystem 作为护盾来源（§15.5 护盾先于 HP）
        (void)bus.Subscribe<combat::BuffApplied>(
            [this](const combat::BuffApplied&) { ++buff_applied; });
        (void)bus.Subscribe<combat::BuffRemoved>(
            [this](const combat::BuffRemoved&) { ++buff_removed; });
        (void)bus.Subscribe<combat::BuffExpired>(
            [this](const combat::BuffExpired&) { ++buff_expired; });
        (void)bus.Subscribe<combat::BuffDispelled>(
            [this](const combat::BuffDispelled&) { ++buff_dispelled; });
    }

    SceneContext Ctx() {
        return SceneContext(kScene, SceneType::World, kOwner, core::MonotonicClock::Point(),
                            tick, mgr, bus, sched, arena);
    }
    SceneContext CtxAt(core::SteadyTime now) {
        return SceneContext(kScene, SceneType::World, kOwner, now, tick, mgr, bus, sched, arena);
    }
    void DrainAll() {
        while (bus.QueueDepth() > 0) (void)bus.Drain();
    }

    EntityId Spawn(CharacterId cid, std::int64_t max_hp, std::int64_t attack = 0,
                   std::int64_t defense = 0, std::int64_t crit_rate = 0,
                   std::int64_t crit_damage = 0) {
        const auto c = roles.LoadOrCreate(static_cast<PlayerId>(cid), cid, Ctx());
        CHECK(c.HasValue());
        Character* cp = roles.Find(cid);
        if (cp == nullptr) return 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Agility)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Strength)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Intellect)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Stamina)] = 0;
        ForceAttr(cp, AttrType::Attack, attack);
        ForceAttr(cp, AttrType::Defense, defense);
        ForceAttr(cp, AttrType::CritRate, crit_rate);
        ForceAttr(cp, AttrType::CritDamage, crit_damage);
        ForceAttr(cp, AttrType::MaxHp, max_hp);
        CHECK(cp->MaxHp() == max_hp);
        cp->hp = max_hp;
        const auto e = mgr.Create(EntityType::Player, kScene, Position{});
        CHECK(e.HasValue());
        if (!e.HasValue()) return 0;
        const EntityId id = e.Value()->Id();
        (void)roles.AttachToScene(cid, id, kScene);
        dmg.BindAvatar(id, cid);
        buffs.BindAvatar(id, cid);
        return id;
    }
};

// ===========================================================================
// 1. 堆叠规则：None / Refresh / Independent
// ===========================================================================
void test_stacking_rules() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId e = h.Spawn(1, 100000);
    const CharacterId cid = 1;
    const core::SteadyTime now = core::MonotonicClock::Point();

    // None：重复施加只刷新，不叠加（Stone Skin, id 1003, stack_rule 0）
    CHECK(h.buffs.Apply(cid, 1003, cid, 1, now).HasValue());
    CHECK(h.buffs.Apply(cid, 1003, cid, 1, now).HasValue());
    CHECK(h.buffs.ActiveBuffCount(cid) == 1);

    // Refresh：层数 +1 直到上限（Strength Aura, id 1001, max_stacks 3）
    CHECK(h.buffs.Apply(cid, 1001, cid, 1, now).Value() == 1);
    CHECK(h.buffs.Apply(cid, 1001, cid, 1, now).Value() == 2);
    CHECK(h.buffs.Apply(cid, 1001, cid, 1, now).Value() == 3);
    CHECK(h.buffs.Apply(cid, 1001, cid, 1, now).Value() == 3);  // 封顶
    CHECK(h.buffs.ActiveBuffCount(cid) == 2);

    // Independent：独立实例，达上限顶掉最旧（Poison, id 1005, max_stacks 3）
    for (int i = 0; i < 5; ++i) CHECK(h.buffs.Apply(cid, 1005, cid, 1, now).HasValue());
    CHECK(h.buffs.ActiveBuffCount(cid) == 5);  // 2 + 3（Poison 封顶 3）
    const auto* psn = h.buffs.BuffsOf(cid);
    std::size_t poison = 0;
    if (psn) for (const auto& inst : *psn) if (inst.def->id == 1005) ++poison;
    CHECK(poison == 3);
}

// ===========================================================================
// 2. 到期清理
// ===========================================================================
void test_expiry() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId e = h.Spawn(1, 100000);
    const CharacterId cid = 1;

    auto now = core::MonotonicClock::Point();
    CHECK(h.buffs.Apply(cid, 1004, cid, 1, now).HasValue());  // Stun, 3000ms
    CHECK(h.buffs.HasControlFlag(cid, ControlFlag::Stunned));
    CHECK(h.buffs.ActiveBuffCount(cid) == 1);

    // 前进 3000ms：尚未到期
    now += std::chrono::milliseconds(2999);
    h.buffs.Tick(h.CtxAt(now));
    CHECK(h.buffs.ActiveBuffCount(cid) == 1);

    // 再前进 1ms：到期
    now += std::chrono::milliseconds(2);
    h.buffs.Tick(h.CtxAt(now));
    CHECK(h.buffs.ActiveBuffCount(cid) == 0);
    CHECK(!h.buffs.HasControlFlag(cid, ControlFlag::Stunned));
    h.DrainAll();
    CHECK(h.buff_expired >= 1);
}

// ===========================================================================
// 3. 周期结算：DOT / HOT（经 DamageSystem 统一结算）
// ===========================================================================
void test_periodic_dot_hot() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId e = h.Spawn(1, 100000, 0, 0);  // 高血防，确保 DOT 期间不死
    const CharacterId cid = 1;
    auto now = core::MonotonicClock::Point();

    // Poison：DOT 30/ tick，1000ms 间隔（id 1005）
    CHECK(h.buffs.Apply(cid, 1005, cid, 1, now).HasValue());
    Character* c = h.roles.Find(cid);
    CHECK(c->hp == 100000);
    for (int i = 0; i < 5; ++i) {
        now += std::chrono::milliseconds(1000);
        h.buffs.Tick(h.CtxAt(now));
        h.DrainAll();
    }
    CHECK(c->hp == 100000 - 150);  // 5 跳 × 30

    // 先移除 Poison（仍在生效），避免 HOT 阶段继续 DOT 干扰
    CHECK(h.buffs.Remove(cid, 1005, RemoveReason::Manual, 1).HasValue());

    // Regeneration：HOT 50/ tick（id 1006），先把血降到 500
    c->hp = 500;
    CHECK(h.buffs.Apply(cid, 1006, cid, 1, now).HasValue());
    for (int i = 0; i < 3; ++i) {
        now += std::chrono::milliseconds(1000);
        h.buffs.Tick(h.CtxAt(now));
        h.DrainAll();
    }
    CHECK(c->hp == 500 + 150);  // 3 跳 × 50，未超 MaxHp（100000）
    CHECK(h.buffs.Stats().ticks >= 8);
}

// ===========================================================================
// 4. 先加后乘：Buff 只写 from_buff 层
// ===========================================================================
void test_apply_before_multiply() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId e = h.Spawn(1, 10000, 0, 0, 0, 0);  // MaxHp 10000（无主属性加成）
    const CharacterId cid = 1;
    auto now = core::MonotonicClock::Point();

    // Strength Aura（id 1001）：modifiers.Strength=50 / Attack=10，multipliers.MaxHp=0.1
    CHECK(h.buffs.Apply(cid, 1001, cid, 1, now).HasValue());
    Character* c = h.roles.Find(cid);
    // MaxHp：110% of 无 Buff 基准（=10000）→ 11000；乘法引用「无 Buff 基准」而非 Final
    CHECK(c->MaxHp() == 11000);
    CHECK(c->attrs.Total(AttrType::Strength) == 50);  // 主属性：纯加法 +50
    // 派生 Attack：Strength 50 → 公式 2*50+5=105，叠加 Buff 加法 +10，基数基准为 0 ⇒ 110
    CHECK(c->attrs.Total(AttrType::Attack) == 110);
    // Buff 只写 from_buff 一层（直接贡献）：Strength +50 / Attack +10
    CHECK(c->attrs.from_buff[static_cast<std::size_t>(AttrType::Strength)] == 50);
    CHECK(c->attrs.from_buff[static_cast<std::size_t>(AttrType::Attack)] == 10);

    // 移除后回归基准
    CHECK(h.buffs.Remove(cid, 1001, RemoveReason::Manual, 1).HasValue());
    CHECK(c->MaxHp() == 10000);
    CHECK(c->attrs.Total(AttrType::Strength) == 0);
    CHECK(c->attrs.from_buff[static_cast<std::size_t>(AttrType::Strength)] == 0);
    CHECK(c->attrs.from_buff[static_cast<std::size_t>(AttrType::Attack)] == 0);
}

// ===========================================================================
// 5. 驱散（含不可驱散）
// ===========================================================================
void test_dispel() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId e = h.Spawn(1, 100000);
    const CharacterId cid = 1;
    auto now = core::MonotonicClock::Point();

    CHECK(h.buffs.Apply(cid, 1001, cid, 1, now).HasValue());  // dispellable
    CHECK(h.buffs.Apply(cid, 1004, cid, 1, now).HasValue());  // Stun, 不可驱散

    CHECK(h.buffs.Dispel(cid, 1001, 1).HasValue());           // 可驱散
    CHECK(h.buffs.ActiveBuffCount(cid) == 1);                 // 仅剩 Stun
    const auto r = h.buffs.Dispel(cid, 1004, 1);              // 不可驱散 → 失败
    CHECK(!r.HasValue());
    if (!r.HasValue()) CHECK(r.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);
    h.DrainAll();
    CHECK(h.buff_dispelled >= 1);
}

// ===========================================================================
// 6. 槽位上限 → BUSY
// ===========================================================================
void test_slot_cap() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId e = h.Spawn(1, 100000);
    const CharacterId cid = 1;
    auto now = core::MonotonicClock::Point();

    // 注册 70 个独立、单层 Buff 定义（id 2000..2069），逐个施加
    for (int i = 0; i < 70; ++i) {
        BuffDef d{};
        d.id = static_cast<std::uint32_t>(2000 + i);
        d.stack_rule = StackRule::None;
        d.max_stacks = 1;
        CHECK(h.registry.Add(std::move(d)).HasValue());
    }
    std::size_t applied = 0;
    for (int i = 0; i < 70; ++i) {
        const auto r = h.buffs.Apply(cid, static_cast<std::uint32_t>(2000 + i), cid, 1, now);
        if (r.HasValue()) ++applied;
        else {
            CHECK(r.Err().Code() == core::ErrorCode::BUSY);
            break;
        }
    }
    CHECK(applied == BuffSystem::kMaxBuffsPerChar);  // 恰好 64
    CHECK(h.buffs.ActiveBuffCount(cid) == BuffSystem::kMaxBuffsPerChar);
}

// ===========================================================================
// 7. 护盾（经 IShieldSource 接入 DamageSystem）
// ===========================================================================
void test_shield() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId e = h.Spawn(1, 100000, 0, 0);
    const CharacterId cid = 1;
    auto now = core::MonotonicClock::Point();

    CHECK(h.buffs.Apply(cid, 1003, cid, 1, now).HasValue());  // Stone Skin, shield 500
    CHECK(h.buffs.ShieldOf(e) == 500);

    h.buffs.ConsumeShield(e, 200);
    CHECK(h.buffs.ShieldOf(e) == 300);
    h.buffs.ConsumeShield(e, 300);
    CHECK(h.buffs.ShieldOf(e) == 0);

    // 真实伤害经 DamageSystem 先吃护盾：先移除（None 规则只刷新时长、不补盾）再施加，得到满盾
    CHECK(h.buffs.Remove(cid, 1003, RemoveReason::Manual, 1).HasValue());
    CHECK(h.buffs.Apply(cid, 1003, cid, 1, now).HasValue());
    Character* c = h.roles.Find(cid);
    c->hp = 100000;
    DamageRequest req{};
    req.source = e;
    req.target = e;
    req.school = DamageSchool::TrueDamage;
    req.base_amount = 300;
    req.coefficient = 0.0f;
    req.trace = 1u;
    const auto r = h.dmg.ApplyDamage(req, h.CtxAt(now));
    CHECK(r.HasValue());
    if (r.HasValue()) {
        CHECK(r.Value().absorbed == 300);
        CHECK(r.Value().final_amount == 0);
        CHECK(c->hp == 100000);  // 护盾覆盖，HP 不变
    }
    CHECK(h.buffs.ShieldOf(e) == 200);  // 500 - 300
}

// ===========================================================================
// 8. 配置化加载 + 未知 buff → NOT_FOUND
// ===========================================================================
void test_config_load() {
    Harness h(LoadedFormula());
    const auto r = h.registry.LoadFromConfig(kBuffConfig);
    CHECK(r.HasValue());
    if (!r.HasValue()) return;
    CHECK(h.registry.Size() == 6);
    const BuffDef* stone = h.registry.Find(1003);
    CHECK(stone != nullptr);
    CHECK(stone->shield_value == 500);
    CHECK(stone->kind == BuffKind::Shield);
    const BuffDef* stun = h.registry.Find(1004);
    CHECK(stun != nullptr);
    CHECK(stun->control_mask == static_cast<std::uint8_t>(ControlFlag::Stunned));
    const BuffDef* aura = h.registry.Find(1001);
    CHECK(aura != nullptr);
    CHECK(aura->attr_multipliers[static_cast<std::size_t>(AttrType::MaxHp)] == 0.1);

    // 未知 buff_id → NOT_FOUND
    const EntityId e = h.Spawn(1, 100000);
    auto now = core::MonotonicClock::Point();
    const auto bad = h.buffs.Apply(1, 999999, 1, 1, now);
    CHECK(!bad.HasValue());
    if (!bad.HasValue()) CHECK(bad.Err().Code() == core::ErrorCode::NOT_FOUND);
}

// ===========================================================================
// 9. 热路径红线：src/buff 无 mysql/redis/grpc/kafka/ifstream/std::thread
// ===========================================================================
void test_hotpath_no_external_io() {
#ifdef _WIN32
    // Git Bash 下 std::filesystem 路径用正斜杠
    const std::string dir = "server/gamenode/combat/src/buff";
#else
    const std::string dir = "server/gamenode/combat/src/buff";
#endif
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        ErrorFmt("FAIL: %s not found (cwd must be repo root)\n", dir.c_str());
        ++g_fail;
        return;
    }
    static const char* kForbidden[] = {"mysql", "redis", "grpc", "kafka", "sql::",
                                       "std::ifstream", "std::ofstream", "std::thread"};
    std::size_t scanned = 0;
    std::size_t hits = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path());
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            for (const char* bad : kForbidden) {
                if (line.find(bad) != std::string::npos) {
                    ErrorFmt("FAIL: forbidden token '%s' in %s: %s\n", bad,
                             entry.path().string().c_str(), line.c_str());
                    ++hits;
                }
            }
        }
        ++scanned;
    }
    CHECK(scanned >= 2);  // buff_system.cpp + buff_config.cpp
    CHECK(hits == 0);
    LineFmt("hotpath scan: %zu files, %zu forbidden tokens\n", scanned, hits);
}

}  // namespace

int main() {
    LineFmt("== TASK-023 buff_test ==\n");
    test_stacking_rules();
    test_expiry();
    test_periodic_dot_hot();
    test_apply_before_multiply();
    test_dispel();
    test_slot_cap();
    test_shield();
    test_config_load();
    test_hotpath_no_external_io();
    LineFmt("buff_test done: fail=%d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
