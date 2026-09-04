// server/gamenode/combat/tests/damage_test.cpp — TASK-022 §16 单元 / §17 集成 / §19 Failure
//
// 覆盖：
//   §16 单元：公式各分支（暴击 / 闪避 / 抗性 / 护盾 / 致死）、边界（0 伤害、超高防御、
//             护盾全覆盖、HP 恰好归零）、治疗溢出、确定性 PRNG 可复现、统计聚合；
//   §17 集成：5 人 vs 20 怪 10000 次伤害结算 —— HP 守恒、无负值、死亡事件数 == 致死次数、
//             统计与逐条重算一致、DamageRecord 回填一致；
//   §19 失败：目标已死 → NOT_FOUND；超高伤害钳制不溢出；NaN 系数 → INVALID_ARGUMENT；
//             PRNG 存档/恢复后序列连续；配置缺失 → 加载失败（禁止默认值静默启动）；
//   §20.3 热路径红线：src/damage 源码不出现 mysql/redis/grpc/kafka/ifstream/ofstream。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/combat/damage/damage.h"
#include "mmo/game/combat/damage/damage_formula.h"
#include "mmo/game/combat/damage/damage_system.h"
#include "mmo/game/combat/damage/prng.h"
#include "mmo/game/combat/skill/combat_events.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_events.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::combat;
using namespace mmo::game::role;

using core::test::ErrorFmt;
using core::test::LineFmt;

constexpr SceneId kScene = 1;
constexpr NodeId kOwner = 1;
constexpr const char* kFormulaPath = "config/gameplay/combat/formula.json";
constexpr const char* kDamageSrcDir = "server/gamenode/combat/src/damage";

// 累计失败数（与 skill_test 同款极简断言框架）
int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                            \
            ++g_fail;                                                            \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                        \
    } while (0)

// ============================================================================
// 测试脚手架
// ============================================================================

/// 固定容量 Arena（帧内 bump 分配，Tick 结束 Reset）。
class FixedArena final : public core::Arena {
public:
    explicit FixedArena(std::size_t bytes) : core::Arena(bytes) {}
};

/// 假护盾源（TASK-023 真实实现的替身，§15.5「本任务只定义接口」）。
class FakeShield final : public IShieldSource {
public:
    explicit FakeShield(std::int64_t v) noexcept : value_(v) {}
    std::int64_t ShieldOf(EntityId) const noexcept override { return value_; }
    void ConsumeShield(EntityId, std::int64_t amount) noexcept override {
        consumed_ += amount;
        if (amount >= value_) value_ = 0;
        else value_ -= amount;
    }
    std::int64_t consumed_{0};
    std::int64_t value_{0};
};

/// 线性经验曲线（回归用，与数值平衡无关）。
ExpCurve MakeCurve() {
    const auto c = ExpCurve::FromConfig(ExpCurveConfig{60, 100.0, 1.0});
    if (!c.HasValue()) {
        ErrorFmt("FATAL: bad exp curve config\n");
        ++g_fail;
        return ExpCurve::FromConfig(ExpCurveConfig{1, 1.0, 1.0}).Value();
    }
    return c.Value();
}

/// 把某个属性的 **Final（Total）** 精确调到 target。
///
/// 为什么需要它：TASK-016 的派生属性（MaxHp/Defense/Attack/CritRate/CritDamage）由主属性
/// 按 `AttrFormula` 计算，直接写 `base[MaxHp]` 只是**加在公式结果上的偏移量**，
/// 并不是设定目标值。先把当前 Total 读回来、算出差值再补进 base，才能得到确定的属性值。
void ForceAttr(Character* c, AttrType t, std::int64_t target) {
    c->attrs.Recompute();
    const std::int64_t cur = c->attrs.Total(t);
    if (cur != target) {
        c->attrs.base[static_cast<std::size_t>(t)] += (target - cur);
        c->attrs.Recompute();
    }
}

struct Harness {
    InMemoryPersistenceAdapter sink;
    core::EventBus bus;
    RoleSystem roles{sink, MakeCurve()};
    EntityManager mgr;
    FixedArena arena{1u << 20};
    core::Scheduler sched;
    DamageSystem dmg;
    std::uint64_t tick{0};

    // 事件计数
    std::size_t damage_events{0};
    std::size_t heal_events{0};
    std::size_t entity_died{0};
    std::size_t character_died{0};

    explicit Harness(const DamageFormula& f) : dmg(f, roles, mgr, kScene, &bus) {
        roles.BindEventBus(bus);
        (void)bus.Subscribe<DamageEvent>([this](const DamageEvent&) { ++damage_events; });
        (void)bus.Subscribe<HealEvent>([this](const HealEvent&) { ++heal_events; });
        (void)bus.Subscribe<EntityDied>([this](const EntityDied&) { ++entity_died; });
        (void)bus.Subscribe<CharacterDied>([this](const CharacterDied&) { ++character_died; });
    }

    SceneContext Ctx() {
        return SceneContext(kScene, SceneType::World, kOwner, core::MonotonicClock::Point(),
                            tick, mgr, bus, sched, arena);
    }
    /// 事件总量远超单次 Drain 上限（4096）时必须循环排空（TASK-021 教训）。
    void DrainAll() {
        while (bus.QueueDepth() > 0) (void)bus.Drain();
    }

    /// 生成一个角色 + 绑定实体。max_hp 同时作为初始 HP。
    EntityId Spawn(CharacterId cid, std::int64_t max_hp, std::int64_t attack = 0,
                   std::int64_t defense = 0, std::int64_t crit_rate = 0,
                   std::int64_t crit_damage = 0) {
        const auto c = roles.LoadOrCreate(static_cast<PlayerId>(cid), cid, Ctx());
        CHECK(c.HasValue());
        Character* cp = roles.Find(cid);
        if (cp == nullptr) {
            ++g_fail;
            return 0;
        }
        // 先清零主属性（影响 CritRate/Defense 的公式基数），再逐个把派生属性顶到目标值。
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
        return id;
    }
};

/// 加载公式（失败即整体失败，禁止默认值静默启动）。
DamageFormula LoadedFormula() {
    auto f = DamageFormula::LoadFromFile(kFormulaPath);
    if (!f.HasValue()) {
        ErrorFmt("FATAL: cannot load %s: %s\n", kFormulaPath, f.Err().ToString().c_str());
        ++g_fail;
        return DamageFormula{};
    }
    return f.Value();
}

// ============================================================================
// 1. 配置加载：字段齐全 / 幂等 / 缺文件失败 / Validate 拒绝非法值
// ============================================================================
void test_formula_config() {
    auto f = DamageFormula::LoadFromFile(kFormulaPath);
    CHECK(f.HasValue());
    if (!f.HasValue()) return;
    const DamageFormula& d = f.Value();
    CHECK(d.rate_scale == 10000);
    CHECK(d.mitigation_k == 400);
    CHECK(d.min_damage == 1);
    CHECK(d.max_raw_damage == 1000000);
    CHECK(d.dodge_rate_max == 5000);
    CHECK(d.tick_rate_hz == 20);

    // 幂等：同一路径重复加载不应失败（ConfigManager 键冲突已按路径规避）
    CHECK(DamageFormula::LoadFromFile(kFormulaPath).HasValue());

    // 缺文件 → 失败（禁止默认值静默生成，也禁止拿上一份配置顶替）
    const auto missing = DamageFormula::LoadFromFile("config/gameplay/combat/__nope__.json");
    CHECK(!missing.HasValue());
    if (!missing.HasValue()) {
        CHECK(missing.Err().Code() == core::ErrorCode::NOT_FOUND);
    }
    // 失败后再次加载正确路径仍应成功（LoadedPath 未被错误路径污染）
    CHECK(DamageFormula::LoadFromFile(kFormulaPath).HasValue());

    // Validate 拒绝非法值
    DamageFormula bad = d;
    bad.rate_scale = 0;
    CHECK(!bad.Validate().HasValue());
    bad = d;
    bad.mitigation_k = -1;
    CHECK(!bad.Validate().HasValue());
    bad = d;
    bad.mitigation_max = d.rate_scale;  // 100% 减免 = 免疫，非法
    CHECK(!bad.Validate().HasValue());
    bad = d;
    bad.dodge_rate_max = d.rate_scale + 1;
    CHECK(!bad.Validate().HasValue());
    bad = d;
    bad.crit_rate_max = d.rate_scale + 1;
    CHECK(!bad.Validate().HasValue());
    CHECK(d.Validate().HasValue());
}

// ============================================================================
// 2. 确定性 PRNG：同种子同序列 / 不同种子不同 / 状态保存恢复后连续（§19 回放）
// ============================================================================
void test_prng_determinism() {
    Prng a(0), b(0);
    a.Seed(kScene, 100, 7);
    b.Seed(kScene, 100, 7);
    bool same = true;
    for (int i = 0; i < 1000; ++i) {
        if (a.Next() != b.Next()) same = false;
    }
    CHECK(same);  // 同 (scene, tick, seq) 完全复现

    Prng c(0);
    c.Seed(kScene, 101, 7);
    Prng d(0);
    d.Seed(kScene, 100, 7);
    CHECK(c.Next() != d.Next());  // Tick 不同 ⇒ 序列不同

    Prng e(0);
    e.Seed(9, 5, 1);
    Prng f(0);
    f.Seed(9, 5, 2);
    CHECK(e.Next() != f.Next());  // 序号不同 ⇒ 序列不同

    // 状态保存 / 恢复后序列**连续**（§19 回放验证）
    Prng g(0);
    g.Seed(kScene, 42, 3);
    for (int i = 0; i < 10; ++i) (void)g.Next();
    const Prng::State snap = g.Save();
    std::vector<std::uint64_t> expect;
    expect.reserve(20);
    for (int i = 0; i < 20; ++i) expect.push_back(g.Next());
    g.Restore(snap);
    bool resumed = true;
    for (int i = 0; i < 20; ++i) {
        if (g.Next() != expect[static_cast<std::size_t>(i)]) resumed = false;
    }
    CHECK(resumed);

    // NextScaled 落在 [0, scale) 且分布不退化（取高 32 位定点缩放，非低位取模）
    Prng h(0);
    h.Seed(kScene, 1, 1);
    std::uint64_t sum = 0;
    std::uint32_t maxv = 0;
    bool in_range = true;
    for (int i = 0; i < 20000; ++i) {
        const std::uint32_t v = h.NextScaled(10000);
        if (v >= 10000) in_range = false;
        sum += v;
        if (v > maxv) maxv = v;
    }
    CHECK(in_range);
    const std::uint64_t mean = sum / 20000;
    CHECK(mean > 4600 && mean < 5400);  // 期望 5000，±8% 容差
    CHECK(maxv > 9000);
    CHECK(h.NextScaled(0) == 0);  // scale=0 防御
}

// ============================================================================
// 3. 公式纯函数：各分支与边界（不依赖 DamageSystem 状态，§15.2）
// ============================================================================
void test_pure_formula_branches() {
    const DamageFormula f = LoadedFormula();

    // raw 钳制：负 → 0，超限 → max_raw_damage
    CHECK(ClampRaw(-5, f) == 0);
    CHECK(ClampRaw(f.max_raw_damage + 1, f) == f.max_raw_damage);
    CHECK(ClampRaw(123, f) == 123);

    // 闪避率：base + per_agility * agility，钳到 max
    CHECK(DodgeRateOf(0, f) == f.dodge_rate_base);
    CHECK(DodgeRateOf(10, f) == f.dodge_rate_base + 10 * f.dodge_per_agility);
    CHECK(DodgeRateOf(1000000, f) == f.dodge_rate_max);

    // 暴击：roll < crit_rate 才爆；伤害 × CritDamage / rate_scale
    bool crit = false;
    CHECK(ApplyCrit(1000, 5000, 15000, 4999, f, &crit) == 1500);
    CHECK(crit);
    crit = false;
    CHECK(ApplyCrit(1000, 5000, 15000, 5000, f, &crit) == 1000);
    CHECK(!crit);
    // crit_rate 超上限被钳到 crit_rate_max(9500)：roll 9000 < 9500 ⇒ 仍然暴击
    crit = false;
    CHECK(ApplyCrit(1000, 999999, 15000, 9000, f, &crit) == 1500);
    CHECK(crit);
    // roll 9600 ≥ crit_rate_max(9500) ⇒ 被上限挡下，不暴击
    crit = false;
    CHECK(ApplyCrit(1000, 999999, 15000, 9600, f, &crit) == 1000);
    CHECK(!crit);
    // crit_rate = 0 ⇒ 永不暴击
    crit = false;
    CHECK(ApplyCrit(1000, 0, 15000, 0, f, &crit) == 1000);
    CHECK(!crit);

    // 闪避
    CHECK(IsDodged(300, 299));
    CHECK(!IsDodged(300, 300));
    CHECK(!IsDodged(0, 0));

    // 抗性减免：raw * K / (K + Defense)
    CHECK(Mitigate(1000, 0, f) == 1000);    // 无护甲
    CHECK(Mitigate(1000, 400, f) == 500);   // 1000*400/800
    CHECK(Mitigate(1000, 1200, f) == 250);  // 1000*400/1600
    // 超高防御触到减免上限：≥ raw 的 25%
    CHECK(Mitigate(1000, 100000000, f) == 250);
    // 保底伤害：raw > 0 但减免后为 0 → min_damage；raw = 0 不给保底
    CHECK(Mitigate(0, 0, f) == 0);
    CHECK(Mitigate(1, 100000000, f) == f.min_damage);

    // 护盾吸收
    std::int64_t absorbed = 0;
    bool blocked = false;
    CHECK(Absorb(500, 0, f, &absorbed, &blocked) == 500);
    CHECK(absorbed == 0);
    CHECK(!blocked);
    CHECK(Absorb(500, 200, f, &absorbed, &blocked) == 300);
    CHECK(absorbed == 200);
    CHECK(!blocked);
    CHECK(Absorb(500, 500, f, &absorbed, &blocked) == 0);
    CHECK(absorbed == 500);
    CHECK(blocked);  // 完全吸收 ⇒ is_blocked
    CHECK(Absorb(500, 900, f, &absorbed, &blocked) == 0);
    CHECK(absorbed == 500);
    CHECK(blocked);

    // 治疗钳制
    std::int64_t eff = 0, over = 0;
    SettleHeal(300, 100, 1000, &eff, &over);
    CHECK(eff == 300);
    CHECK(over == 0);
    SettleHeal(300, 800, 1000, &eff, &over);
    CHECK(eff == 200);
    CHECK(over == 100);
    SettleHeal(300, 1000, 1000, &eff, &over);
    CHECK(eff == 0);
    CHECK(over == 300);
}

// ============================================================================
// 4. 结算顺序固定（§20.2）：暴击 → 闪避 → 抗性 → 护盾 → 扣血 → 致死 → 事件
// ============================================================================
void test_settlement_order() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    // 攻方：Attack 100、必爆（CritRate 10000）、暴击伤害 2.0x
    const EntityId src = h.Spawn(1, 10000, 100, 0, 10000, 20000);
    // 守方：Defense 400、无敏捷（闪避率 = base 200）
    const EntityId dst = h.Spawn(2, 100000, 0, 400, 0, 0);
    CHECK(src != 0 && dst != 0);

    Character* sc = h.roles.Find(1);
    Character* dc = h.roles.Find(2);

    DamageRequest req{};
    req.source = src;
    req.target = dst;
    req.school = DamageSchool::Physical;
    req.base_amount = 0;
    req.coefficient = 1.0f;  // raw = 0 + 1.0 * 100 = 100
    req.trace = 1u;

    // ---- 顺序断言 1：暴击先于抗性 ----
    // raw 100 → 暴击 ×2.0 = 200 → 减免 200*400/800 = 100
    {
        DamageRolls rolls{};
        rolls.crit = 0;      // < 10000 ⇒ 必爆
        rolls.dodge = 9999;  // ≥ dodge_rate(200) ⇒ 不闪避
        const DamageResult r = h.dmg.ComputeDamage(req, sc->attrs, dc->attrs, rolls);
        CHECK(r.raw == 100);
        CHECK(r.is_crit);
        CHECK(!r.is_dodged);
        CHECK(r.mitigated == 100);
        CHECK(r.final_amount == 100);
    }

    // ---- 顺序断言 2：闪避先于抗性与护盾（闪避时 mitigated/absorbed/final 全 0）----
    {
        DamageRolls rolls{};
        rolls.crit = 0;
        rolls.dodge = 0;  // < 200 ⇒ 必闪避
        const DamageResult r = h.dmg.ComputeDamage(req, sc->attrs, dc->attrs, rolls);
        CHECK(r.is_dodged);
        CHECK(r.mitigated == 0);
        CHECK(r.absorbed == 0);
        CHECK(r.final_amount == 0);
        CHECK(!r.is_blocked);
    }

    // ---- 顺序断言 3：护盾全覆盖时 HP 不掉（护盾先于 HP）----
    {
        FakeShield shield(60);
        h.dmg.SetShieldSource(&shield);
        req.can_crit = false;
        req.can_be_dodged = false;
        const auto applied = h.dmg.ApplyDamage(req, h.Ctx());
        CHECK(applied.HasValue());
        if (applied.HasValue()) {
            const DamageResult r = applied.Value();
            // 未暴击：raw 100 → 减免 100*400/800 = 50 → 护盾 60 > 50 ⇒ 全覆盖
            CHECK(r.raw == 100);
            CHECK(r.mitigated == 50);
            CHECK(r.absorbed == 50);
            CHECK(r.final_amount == 0);
            CHECK(r.is_blocked);
            CHECK(r.remaining_hp == 100000);  // HP 未掉
        }
        CHECK(shield.consumed_ == 50);
        h.dmg.SetShieldSource(nullptr);
    }

    // ---- 顺序断言 4：护盾不足时先吃护盾再扣血 ----
    {
        Character* d2 = h.roles.Find(2);
        d2->hp = 100000;
        FakeShield shield(20);
        h.dmg.SetShieldSource(&shield);
        req.can_crit = false;
        req.can_be_dodged = false;
        const auto applied = h.dmg.ApplyDamage(req, h.Ctx());
        CHECK(applied.HasValue());
        if (applied.HasValue()) {
            const DamageResult r = applied.Value();
            CHECK(r.raw == 100);
            CHECK(r.mitigated == 50);
            CHECK(r.absorbed == 20);
            CHECK(r.final_amount == 30);
            CHECK(!r.is_blocked);
            CHECK(r.remaining_hp == 99970);
        }
        CHECK(shield.consumed_ == 20);
        CHECK(shield.value_ == 0);
        h.dmg.SetShieldSource(nullptr);
    }

    // ---- 顺序断言 5：伤害事件已发布，未致死则无 EntityDied ----
    h.DrainAll();
    CHECK(h.damage_events == 2);
    CHECK(h.entity_died == 0);
}

// ============================================================================
// 5. 护盾全覆盖：HP 不变、is_blocked、护盾逐次耗尽（§15.5）
// ============================================================================
void test_shield_full_absorb() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId src = h.Spawn(1, 10000, 0, 0);
    const EntityId dst = h.Spawn(2, 5000, 0, 0);
    CHECK(src != 0 && dst != 0);

    FakeShield shield(10000);  // 护盾远大于单次伤害
    h.dmg.SetShieldSource(&shield);

    DamageRequest req{};
    req.source = src;
    req.target = dst;
    req.school = DamageSchool::Magical;
    req.base_amount = 500;
    req.coefficient = 0.0f;
    req.can_crit = false;
    req.can_be_dodged = false;
    req.trace = 2u;

    const auto r = h.dmg.ApplyDamage(req, h.Ctx());
    CHECK(r.HasValue());
    if (r.HasValue()) {
        CHECK(r.Value().raw == 500);
        CHECK(r.Value().mitigated == 500);   // Defense 0 ⇒ 不减免
        CHECK(r.Value().absorbed == 500);
        CHECK(r.Value().final_amount == 0);
        CHECK(r.Value().is_blocked);
        CHECK(r.Value().remaining_hp == 5000);  // HP 未掉
    }
    CHECK(shield.value_ == 9500);

    // 连续打光护盾（10000 / 500 = 20 次）
    for (int i = 0; i < 19; ++i) {
        const auto rr = h.dmg.ApplyDamage(req, h.Ctx());
        CHECK(rr.HasValue());
    }
    CHECK(shield.value_ == 0);
    CHECK(shield.consumed_ == 10000);
    // 护盾耗尽后的第 21 次开始真正掉血
    const auto after = h.dmg.ApplyDamage(req, h.Ctx());
    CHECK(after.HasValue());
    if (after.HasValue()) {
        CHECK(after.Value().absorbed == 0);
        CHECK(after.Value().final_amount == 500);
        CHECK(after.Value().remaining_hp == 4500);
    }

    h.DrainAll();
    CHECK(h.damage_events == 21);
    h.dmg.SetShieldSource(nullptr);
}

// ============================================================================
// 6. 致死：HP 恰好归零 / EntityDied 只发一次 / 已死目标再打 → NOT_FOUND（§19）
// ============================================================================
void test_lethal_and_dead_target() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId src = h.Spawn(1, 10000, 0, 0);
    const EntityId dst = h.Spawn(2, 1000, 0, 0);
    CHECK(src != 0 && dst != 0);

    DamageRequest req{};
    req.source = src;
    req.target = dst;
    req.school = DamageSchool::TrueDamage;  // 真伤：不减免、不可闪避
    req.base_amount = 1000;                 // 恰好归零
    req.coefficient = 0.0f;
    req.can_crit = false;
    req.trace = 3u;

    const auto r = h.dmg.ApplyDamage(req, h.Ctx());
    CHECK(r.HasValue());
    if (r.HasValue()) {
        CHECK(r.Value().final_amount == 1000);
        CHECK(r.Value().remaining_hp == 0);
        CHECK(r.Value().lethal);
    }

    // 已死亡目标再打：NOT_FOUND，不产生负 HP、不改任何状态
    const auto again = h.dmg.ApplyDamage(req, h.Ctx());
    CHECK(!again.HasValue());
    if (!again.HasValue()) CHECK(again.Err().Code() == core::ErrorCode::NOT_FOUND);

    DamageRequest overkill{};
    overkill.source = src;
    overkill.target = dst;
    overkill.school = DamageSchool::TrueDamage;
    overkill.base_amount = 999999;
    overkill.coefficient = 0.0f;
    overkill.can_crit = false;
    overkill.trace = 4u;
    const auto oc = h.dmg.ApplyDamage(overkill, h.Ctx());
    CHECK(!oc.HasValue());
    if (!oc.HasValue()) CHECK(oc.Err().Code() == core::ErrorCode::NOT_FOUND);
    CHECK(h.roles.Find(2)->hp == 0);  // 不穿负

    h.DrainAll();
    CHECK(h.entity_died == 1);      // 战斗侧死亡事件**只发一次**
    CHECK(h.character_died == 1);   // Role 侧同样只发一次（TASK-016 §15.7）
    CHECK(h.dmg.Stats().lethal_count == 1);
}

// ============================================================================
// 7. 超高伤害钳制（int64 上溢）与 0 伤害（§19）
// ============================================================================
void test_overflow_and_zero_damage() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId src = h.Spawn(1, 100000, 0, 0);
    const EntityId big_hp = h.Spawn(2, 1000000, 0, 0);  // 承受超高伤害
    const EntityId zero_t = h.Spawn(3, 5000, 0, 0);     // 单独跑 0 伤害，避免被上一条打死
    CHECK(src != 0 && big_hp != 0 && zero_t != 0);

    // base 已是 int64 极大值：raw 必须被钳到 max_raw_damage，不得溢出
    DamageRequest big{};
    big.source = src;
    big.target = big_hp;
    big.school = DamageSchool::Physical;
    big.base_amount = std::numeric_limits<std::int64_t>::max() / 2;
    big.coefficient = 1.0f;
    big.can_crit = false;
    big.trace = 5u;
    const auto rb = h.dmg.ApplyDamage(big, h.Ctx());
    CHECK(rb.HasValue());
    if (rb.HasValue()) {
        CHECK(rb.Value().raw <= f.max_raw_damage);
        CHECK(rb.Value().final_amount <= f.max_raw_damage);
        CHECK(rb.Value().remaining_hp >= 0);
    }

    // 0 伤害：raw = 0 ⇒ 不减免、不扣血，但事件照发（便于观测「打了但没伤害」）
    DamageRequest zero{};
    zero.source = src;
    zero.target = zero_t;
    zero.school = DamageSchool::Physical;
    zero.base_amount = 0;
    zero.coefficient = 0.0f;
    zero.can_crit = false;
    zero.can_be_dodged = false;
    zero.trace = 6u;
    const std::int64_t hp_before = h.roles.Find(3)->hp;
    const auto rz = h.dmg.ApplyDamage(zero, h.Ctx());
    CHECK(rz.HasValue());
    if (rz.HasValue()) {
        CHECK(rz.Value().raw == 0);
        CHECK(rz.Value().mitigated == 0);
        CHECK(rz.Value().final_amount == 0);
        CHECK(rz.Value().remaining_hp == hp_before);
    }

    // 负 base_amount → INVALID_ARGUMENT（禁止「负伤害 = 治疗」这种隐式语义）
    DamageRequest neg{};
    neg.source = src;
    neg.target = zero_t;
    neg.base_amount = -100;
    neg.trace = 7u;
    const auto rn = h.dmg.ApplyDamage(neg, h.Ctx());
    CHECK(!rn.HasValue());
    if (!rn.HasValue()) CHECK(rn.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);
}

// ============================================================================
// 8. NaN / Inf 系数 → INVALID_ARGUMENT（§19）
// ============================================================================
void test_nan_coefficient() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId src = h.Spawn(1, 1000, 0, 0);
    const EntityId dst = h.Spawn(2, 1000, 0, 0);
    CHECK(src != 0 && dst != 0);

    DamageRequest req{};
    req.source = src;
    req.target = dst;
    req.base_amount = 100;
    req.coefficient = std::numeric_limits<float>::quiet_NaN();
    req.trace = 8u;
    const auto r = h.dmg.ApplyDamage(req, h.Ctx());
    CHECK(!r.HasValue());
    if (!r.HasValue()) CHECK(r.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);

    req.coefficient = std::numeric_limits<float>::infinity();
    const auto r2 = h.dmg.ApplyDamage(req, h.Ctx());
    CHECK(!r2.HasValue());
    if (!r2.HasValue()) CHECK(r2.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);

    // 治疗侧同样拒绝
    HealRequest hr{};
    hr.source = src;
    hr.target = dst;
    hr.base_amount = 100;
    hr.coefficient = std::numeric_limits<float>::quiet_NaN();
    hr.trace = 9u;
    const auto rh = h.dmg.ApplyHeal(hr, h.Ctx());
    CHECK(!rh.HasValue());
    if (!rh.HasValue()) CHECK(rh.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);
}

// ============================================================================
// 9. 治疗：溢出钳制 / 满血不刷统计 / 暴击 / 死亡目标不可治（§19）
// ============================================================================
void test_heal_overheal() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    // 治疗者：Attack 100、必爆（CritRate 10000）、暴击伤害 2.0x
    const EntityId src = h.Spawn(1, 1000, 100, 0, 10000, 20000);
    const EntityId dst = h.Spawn(2, 500, 0, 0);
    CHECK(src != 0 && dst != 0);
    Character* dp = h.roles.Find(2);
    dp->hp = 400;  // 缺口 100

    HealRequest req{};
    req.source = src;
    req.target = dst;
    req.base_amount = 300;
    req.coefficient = 0.0f;
    req.can_crit = false;
    req.trace = 10u;

    const auto r = h.dmg.ApplyHeal(req, h.Ctx());
    CHECK(r.HasValue());
    if (r.HasValue()) {
        CHECK(r.Value().raw == 300);
        CHECK(r.Value().effective == 100);  // 只补到满
        CHECK(r.Value().overheal == 200);
        CHECK(r.Value().remaining_hp == 500);
    }
    CHECK(dp->hp == 500);

    // 满血再治：overheal_allowed = false（默认）⇒ 不发布事件、不计入有效治疗量
    const std::size_t heal_before = h.dmg.Stats().heal_events;
    const auto r2 = h.dmg.ApplyHeal(req, h.Ctx());
    CHECK(r2.HasValue());
    if (r2.HasValue()) {
        CHECK(r2.Value().effective == 0);
        CHECK(r2.Value().overheal == 300);
    }
    CHECK(h.dmg.Stats().heal_events == heal_before);

    // overheal_allowed = true ⇒ 计入，溢出量进 total_overheal
    req.overheal_allowed = true;
    const auto r3 = h.dmg.ApplyHeal(req, h.Ctx());
    CHECK(r3.HasValue());
    CHECK(h.dmg.Stats().heal_events == heal_before + 1);
    CHECK(h.dmg.Stats().total_overheal >= 500);

    // 暴击治疗
    dp->hp = 100;
    HealRequest cr{};
    cr.source = src;
    cr.target = dst;
    cr.base_amount = 100;
    cr.coefficient = 0.0f;
    cr.can_crit = true;
    cr.trace = 11u;
    const auto rc = h.dmg.ApplyHeal(cr, h.Ctx());
    CHECK(rc.HasValue());
    if (rc.HasValue()) {
        CHECK(rc.Value().is_crit);
        CHECK(rc.Value().raw == 200);
        CHECK(rc.Value().effective == 200);
        CHECK(rc.Value().remaining_hp == 300);
    }

    // 死亡目标不可治疗
    dp->hp = 0;
    const auto rd = h.dmg.ApplyHeal(cr, h.Ctx());
    CHECK(!rd.HasValue());
    if (!rd.HasValue()) CHECK(rd.Err().Code() == core::ErrorCode::NOT_FOUND);

    h.DrainAll();
    CHECK(h.heal_events > 0);
}

// ============================================================================
// 10. 统计聚合正确（§15.7，平衡性分析必需）
// ============================================================================
void test_stats_aggregation() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId src = h.Spawn(1, 100000, 0, 0);
    const EntityId dst = h.Spawn(2, 10000000, 0, 0);
    CHECK(src != 0 && dst != 0);

    DamageRequest req{};
    req.source = src;
    req.target = dst;
    req.school = DamageSchool::TrueDamage;  // 真伤：不减免、不可闪避
    req.base_amount = 10;
    req.coefficient = 0.0f;
    req.can_crit = false;
    req.trace = 12u;

    for (int i = 0; i < 100; ++i) {
        const auto r = h.dmg.ApplyDamage(req, h.Ctx());
        CHECK(r.HasValue());
    }
    const DamageStats s = h.dmg.Stats();
    CHECK(s.damage_events == 100);
    CHECK(s.hit_count == 100);
    CHECK(s.dodge_count == 0);
    CHECK(s.crit_count == 0);
    CHECK(s.total_final == 1000);
    CHECK(s.AvgDamageX10000() == 10 * 10000);
    CHECK(s.CritRateBp() == 0);
    CHECK(s.DodgeRateBp() == 0);

    // 重置后再打，计数从头开始
    h.dmg.ResetStats();
    CHECK(h.dmg.Stats().damage_events == 0);
    const auto r = h.dmg.ApplyDamage(req, h.Ctx());
    CHECK(r.HasValue());
    CHECK(h.dmg.Stats().damage_events == 1);
    CHECK(h.dmg.Stats().total_final == 10);
}

// ============================================================================
// 11. 采样日志：不每条全写（§15.8 / §20.6「日志量可测」）
// ============================================================================
void test_sampled_log() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);
    const EntityId src = h.Spawn(1, 100000, 0, 0);
    const EntityId dst = h.Spawn(2, 5000000, 0, 0);  // MaxHp 上限 10'000'000（TASK-016 钳制）
    CHECK(src != 0 && dst != 0);

    DamageRequest req{};
    req.source = src;
    req.target = dst;
    req.school = DamageSchool::TrueDamage;
    req.base_amount = 1;
    req.coefficient = 0.0f;
    req.can_crit = false;
    req.trace = 13u;

    // 同一 Tick 内 1000 次结算：每 Tick 预算 = log_samples_per_second / tick_rate_hz = 100/20 = 5
    for (int i = 0; i < 1000; ++i) {
        const auto r = h.dmg.ApplyDamage(req, h.Ctx());
        CHECK(r.HasValue());
    }
    CHECK(h.dmg.Stats().damage_events == 1000);
    CHECK(h.dmg.Stats().log_records == 5);  // 只采样 5 条，其余全部不写日志

    const auto samples = h.dmg.TakeSamples();
    CHECK(samples.size() == 5);
    CHECK(h.dmg.TakeSamples().empty());  // 取走后清空

    // 推进到下一个 Tick，预算刷新
    ++h.tick;
    for (int i = 0; i < 3; ++i) {
        const auto r = h.dmg.ApplyDamage(req, h.Ctx());
        CHECK(r.HasValue());
    }
    CHECK(h.dmg.Stats().log_records == 8);
    CHECK(h.dmg.TakeSamples().size() == 3);
}

// ============================================================================
// 12. 集成：5 人 vs 20 怪 10000 次结算（§17）
//     HP 守恒、无负值、统计与逐条重算一致、DamageRecord 回填一致
// ============================================================================
void test_integration_10000() {
    const DamageFormula f = LoadedFormula();
    Harness h(f);

    constexpr int kPlayers = 5;
    constexpr int kMonsters = 20;
    constexpr int kIters = 10000;

    std::vector<EntityId> players;
    std::vector<EntityId> monsters;
    // 施法者：Attack 60、暴击率 5%（500/10000）、暴击伤害 1.5x
    for (int i = 0; i < kPlayers; ++i) {
        players.push_back(h.Spawn(static_cast<CharacterId>(1000 + i), 200000, 60, 0, 500, 15000));
    }
    // 怪物：Defense 10、无暴击；血量 500000 确保 500 次命中内不死（可验证 HP 守恒）
    for (int i = 0; i < kMonsters; ++i) {
        monsters.push_back(h.Spawn(static_cast<CharacterId>(2000 + i), 500000, 0, 10, 0, 0));
    }

    std::int64_t hp_sum_before = 0;
    for (int i = 0; i < kMonsters; ++i) hp_sum_before += h.roles.Find(2000 + i)->hp;

    std::int64_t expect_final_sum = 0;
    std::size_t expect_lethal = 0;
    std::uint64_t expect_dodged = 0;

    DamageRequest req{};
    req.school = DamageSchool::Physical;
    req.base_amount = 50;
    req.coefficient = 1.5f;  // raw = 50 + 1.5 * 60 = 140
    req.can_crit = true;
    req.can_be_dodged = true;
    req.trace = 100u;

    for (int i = 0; i < kIters; ++i) {
        req.source = players[static_cast<std::size_t>(i % kPlayers)];
        req.target = monsters[static_cast<std::size_t>(i % kMonsters)];
        req.request_id = static_cast<core::RequestID>(i + 1);

        DamageRecord rec{};
        const auto r = h.dmg.ApplyDamage(req, h.Ctx(), &rec);
        CHECK(r.HasValue());
        if (!r.HasValue()) continue;
        const DamageResult d = r.Value();
        CHECK(d.final_amount >= 0);
        CHECK(d.remaining_hp >= 0);
        CHECK(d.mitigated >= d.absorbed);
        CHECK(d.final_amount == d.mitigated - d.absorbed);
        if (d.is_dodged) {
            ++expect_dodged;
            CHECK(d.final_amount == 0);
        } else {
            expect_final_sum += d.final_amount;
            if (d.lethal) ++expect_lethal;
        }
        // 完整记录（out 参数）与返回值一致
        CHECK(rec.result.final_amount == d.final_amount);
        CHECK(rec.target == req.target);
        CHECK(rec.tick_number == h.tick);
    }

    std::int64_t hp_sum_after = 0;
    for (int i = 0; i < kMonsters; ++i) {
        Character* c = h.roles.Find(2000 + i);
        CHECK(c->hp >= 0);
        hp_sum_after += c->hp;
    }

    // HP 守恒：无致死 ⇒ 总扣血严格等于 Σ final_amount
    CHECK(expect_lethal == 0);
    CHECK(hp_sum_before - hp_sum_after == expect_final_sum);

    const DamageStats s = h.dmg.Stats();
    CHECK(s.damage_events == static_cast<std::uint64_t>(kIters));
    CHECK(s.dodge_count == expect_dodged);
    CHECK(s.total_final == static_cast<std::uint64_t>(expect_final_sum));
    CHECK(s.lethal_count == 0);
    CHECK(s.AvgDamageX10000() > 0);

    h.DrainAll();
    CHECK(h.damage_events == static_cast<std::size_t>(kIters));
    CHECK(h.entity_died == 0);

    // 暴击率观测值：面板 5%，但闪避样本也占分母，故观测值略低于 5%。
    // 这里只做量级校验，精确分布由 benchmark 承担。
    CHECK(s.crit_count > 0);
    const std::int64_t crit_bp = s.CritRateBp();
    CHECK(crit_bp > 200 && crit_bp < 600);
    const std::int64_t dodge_bp = s.DodgeRateBp();
    CHECK(dodge_bp >= 0 && dodge_bp < 500);  // 期望 ≈ 200（2%）
}

// ============================================================================
// 13. 热路径红线：src/damage 源码不出现外部 IO 符号（§20.3）
// ============================================================================
void test_hotpath_no_external_io() {
    namespace fs = std::filesystem;
    const fs::path dir(kDamageSrcDir);
    if (!fs::exists(dir)) {
        ErrorFmt("FAIL: %s not found (cwd must be repo root)\n", kDamageSrcDir);
        ++g_fail;
        return;
    }
    static const char* kForbidden[] = {"mysql", "redis", "grpc", "kafka", "sql::",
                                       "std::ifstream", "std::ofstream"};
    std::size_t scanned = 0;
    std::size_t hits = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path());  // 测试侧读文件，与被测代码无关
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
    CHECK(scanned >= 2);  // 至少 damage_formula.cpp + damage_system.cpp
    CHECK(hits == 0);
    LineFmt("hotpath scan: %zu files, %zu forbidden tokens\n", scanned, hits);
}

}  // namespace

int main() {
    LineFmt("== TASK-022 damage_test ==\n");
    test_formula_config();
    test_prng_determinism();
    test_pure_formula_branches();
    test_settlement_order();
    test_shield_full_absorb();
    test_lethal_and_dead_target();
    test_overflow_and_zero_damage();
    test_nan_coefficient();
    test_heal_overheal();
    test_stats_aggregation();
    test_sampled_log();
    test_integration_10000();
    test_hotpath_no_external_io();
    LineFmt("damage_test done: fail=%d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
