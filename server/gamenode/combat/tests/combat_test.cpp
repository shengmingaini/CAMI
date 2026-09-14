/// TASK-024 · CombatSystem 测试（§16 单元 / §17 集成 / §19 Failure / §20 验收）。
///
/// 覆盖：仇恨表单元（溢出淘汰最低）、位标记、仇恨累积与 Top 切换、嘲讽(Scale)、
/// 脱战计时、打断、控制 Buff 联动、死亡清理、无效实体、5v5 集成战斗。
///
/// 输出经 test_print.h（禁止裸 cout/printf，§输出规范）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/combat/buff/buff_def.h"
#include "mmo/game/combat/buff/buff_system.h"
#include "mmo/game/combat/combat_entity.h"
#include "mmo/game/combat/combat_system.h"
#include "mmo/game/combat/damage/damage.h"
#include "mmo/game/combat/damage/damage_formula.h"
#include "mmo/game/combat/damage/damage_system.h"
#include "mmo/game/combat/skill/combat_events.h"
#include "mmo/game/combat/skill/skill_def.h"
#include "mmo/game/combat/skill/skill_system.h"
#include "mmo/game/combat/threat_table.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_events.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"

namespace {

using namespace mmo::game;
using namespace mmo::game::combat;
using namespace mmo::game::role;
using namespace mmo::game::combat::buff;
namespace core = mmo::core;
using core::test::ErrorFmt;

constexpr SceneId kScene = 1;
constexpr NodeId kOwner = 1;
constexpr const char* kFormulaPath = "config/gameplay/combat/formula.json";
constexpr const char* kBuffPath = "config/gameplay/buffs/buffs.json";
constexpr const char* kSkillDir = "config/gameplay/skills";

int g_fail = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ErrorFmt("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);  \
            ++g_fail;                                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

// 前向声明：供 Harness::formula 的默认成员初始化器引用（定义见文件末尾）。
DamageFormula LoadedFormula();

class FixedArena final : public core::Arena {
public:
    explicit FixedArena(std::size_t bytes) : core::Arena(bytes) {}
};

class Harness {
public:
    InMemoryPersistenceAdapter sink;
    core::EventBus bus;
    RoleSystem roles{sink, ExpCurve::FromConfig(ExpCurveConfig{60, 100.0, 1.0}).Value()};
    EntityManager mgr;
    FixedArena arena{1u << 20};
    core::Scheduler sched;
    DamageFormula formula{LoadedFormula()};
    DamageSystem dmg;
    buff::BuffRegistry registry;        // BuffSystem 运行时注册表（LoadFromConfig）
    combat::BuffRegistry skill_buffs;   // SkillSystem 加载期校验用（Load(json_text)）
    buff::BuffSystem buffs;
    SkillSystem skills;
    CombatSystem combat;

    std::size_t damage_events{0};
    std::size_t heal_events{0};
    std::size_t skill_interrupted{0};
    std::size_t entity_died{0};

    explicit Harness()
        : dmg(formula, roles, mgr, kScene, &bus),
          buffs(registry, roles, &dmg, &bus),
          skills(roles, mgr, nullptr, kScene, kOwner, &bus),
          combat(skills, dmg, buffs, roles, mgr, kScene, &bus) {
        roles.BindEventBus(bus);
        (void)bus.Subscribe<DamageEvent>([this](const DamageEvent&) { ++damage_events; });
        (void)bus.Subscribe<HealEvent>([this](const HealEvent&) { ++heal_events; });
        (void)bus.Subscribe<SkillInterrupted>(
            [this](const SkillInterrupted&) { ++skill_interrupted; });
        (void)bus.Subscribe<EntityDied>([this](const EntityDied&) { ++entity_died; });
        combat.BindEventBus(bus);
        // 配置加载（失败即整体失败，禁止静默）
        CHECK(registry.LoadFromConfig(kBuffPath).HasValue());
        // 技能加载期校验引用到的 Buff：读 buffs.json 进 combat::BuffRegistry
        {
            std::ifstream in(kBuffPath, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            CHECK(skill_buffs.Load(ss.str()).HasValue());
        }
        CHECK(skills.LoadSkillsFromDir(kSkillDir, skill_buffs).HasValue());
    }

    SceneContext Ctx(core::SteadyTime now = core::MonotonicClock::Point(),
                     std::uint64_t tick = 0) {
        return SceneContext(kScene, SceneType::World, kOwner, now, tick, mgr, bus, sched, arena);
    }
    void DrainAll() {
        while (bus.QueueDepth() > 0) (void)bus.Drain();
    }

    EntityId Spawn(CharacterId cid, std::int64_t max_hp, std::int64_t attack = 0,
                   std::int64_t defense = 0, EntityType type = EntityType::Player) {
    const auto c = roles.LoadOrCreate(static_cast<PlayerId>(cid), cid, Ctx());
    if (!c.HasValue()) { ErrorFmt("Spawn LoadOrCreate failed (cid=%llu)\n", static_cast<unsigned long long>(cid)); ++g_fail; return 0; }
        Character* cp = roles.Find(cid);
        if (cp == nullptr) { ++g_fail; return 0; }
        cp->attrs.base[static_cast<std::size_t>(AttrType::Agility)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Strength)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Intellect)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Stamina)] = 0;
        // 派生属性顶到目标值（TASK-016 ForceAttr 等价内联）
        auto force = [&](AttrType t, std::int64_t target) {
            cp->attrs.Recompute();
            std::int64_t cur = cp->attrs.Total(t);
            if (cur != target) {
                cp->attrs.base[static_cast<std::size_t>(t)] += (target - cur);
                cp->attrs.Recompute();
            }
        };
        force(AttrType::Attack, attack);
        force(AttrType::Defense, defense);
        force(AttrType::MaxHp, max_hp);
        if (cp->MaxHp() != max_hp) { ErrorFmt("Spawn MaxHp mismatch (cid=%llu)\n", static_cast<unsigned long long>(cid)); ++g_fail; return 0; }
        cp->hp = max_hp;
        cp->mp = 1000;  // 技能耗蓝校验（§15 资源门）；测试不关心真实蓝量，给足即可
        const auto e = mgr.Create(type, kScene, Position{});
        if (!e.HasValue()) { ErrorFmt("Spawn Create failed (cid=%llu)\n", static_cast<unsigned long long>(cid)); ++g_fail; return 0; }
        const EntityId id = e.Value()->Id();
        (void)roles.AttachToScene(cid, id, kScene);
        dmg.BindAvatar(id, cid);
        buffs.BindAvatar(id, cid);
        skills.BindAvatar(id, cid);
        combat.BindAvatar(id, cid);
        return id;
    }

    /// 施放技能并派发事件（使 CombatSystem 的仇恨订阅生效）。
    CastResult Cast(EntityId caster, SkillId skill, EntityId target, core::SteadyTime now) {
        CastRequest req{};
        req.caster = caster;
        req.skill = skill;
        req.target = target;
        req.trace = 1;
        auto r = combat.CastSkill(req, Ctx(now));
        DrainAll();
        return r.HasValue() ? r.Value() : CastResult::Interrupted;
    }
};

DamageFormula LoadedFormula() {
    auto f = DamageFormula::LoadFromFile(kFormulaPath);
    if (!f.HasValue()) {
        ErrorFmt("FATAL: cannot load %s\n", kFormulaPath);
        ++g_fail;
        return DamageFormula{};
    }
    return f.Value();
}

// ===========================================================================
// 1. 仇恨表单元：Add / Remove / Scale / Top / 溢出淘汰最低
// ===========================================================================
void test_threat_table_unit() {
    ThreatTable t;
    CHECK(t.Size() == 0);
    CHECK(!t.Top().has_value());
    t.Add(10, 100);
    t.Add(20, 50);
    t.Add(30, 200);
    CHECK(t.Size() == 3);
    CHECK(t.Top() == std::optional<EntityId>(30));  // 最高威胁
    t.Add(10, 60);  // 累加：10 → 160，但 30(=200) 仍最高
    CHECK(t.Top() == std::optional<EntityId>(30));
    t.Remove(10);
    CHECK(t.Size() == 2);
    CHECK(t.Top() == std::optional<EntityId>(30));
    t.Scale(30, 0.1);  // 30 → 20，低于 20(50)
    CHECK(t.Top() == std::optional<EntityId>(20));

    // 溢出：填满 16 条后再 Add，应淘汰最低威胁而非崩溃 / 无界增长
    ThreatTable big;
    for (int i = 0; i < 16; ++i) big.Add(1000 + i, static_cast<std::int64_t>(i * 10));
    CHECK(big.Size() == 16);
    big.Add(9999, 5);  // 最低威胁是 1000(=0)，应被淘汰
    CHECK(big.Size() == 16);
    // 1000 被淘汰，9999 在表内
    bool has_1000 = false, has_9999 = false;
    for (std::size_t i = 0; i < big.Size(); ++i) {
        if (big.At(i).source == 1000) has_1000 = true;
        if (big.At(i).source == 9999) has_9999 = true;
    }
    CHECK(!has_1000);
    CHECK(has_9999);
    big.Clear();
    CHECK(big.Size() == 0);
}

// ===========================================================================
// 2. 位标记：进入战斗置 InCombat；离开清标志
// ===========================================================================
void test_combat_flags() {
    Harness h;
    auto now = core::MonotonicClock::Point();
    auto a = h.Spawn(1, 1000);
    auto b = h.Spawn(2, 1000);
    (void)h.combat.Update(h.Ctx(now));  // 建立 now_
    CHECK(h.combat.EnterCombat(a, b, 1).HasValue());
    CHECK(h.combat.HasFlag(a, CombatFlag::InCombat));
    CHECK(h.combat.HasFlag(b, CombatFlag::InCombat));  // 互殴
    CHECK(h.combat.LeaveCombat(a, LeaveCombatReason::Manual, 1).HasValue());
    CHECK(!h.combat.HasFlag(a, CombatFlag::InCombat));
}

// ===========================================================================
// 3. 仇恨累积与 Top 切换（事件驱动，§8）
// ===========================================================================
void test_threat_accumulate() {
    Harness h;
    auto now = core::MonotonicClock::Point();
    auto tank = h.Spawn(1, 5000);                  // Player
    auto mob = h.Spawn(2, 3000, 0, 0, EntityType::Monster);  // Monster：否则 SingleTarget 互殴同类型→InvalidTarget
    (void)h.combat.Update(h.Ctx(now));
    CHECK(h.combat.EnterCombat(tank, mob, 1).HasValue());
    CHECK(h.combat.EnterCombat(mob, tank, 1).HasValue());
    // tank 打 mob：伤害事件 → 仇恨表累加（§8 事件驱动）
    h.Cast(tank, 1001, mob, now);  // Fireball（瞬发、无冷却冲突：单次）
    CHECK(h.damage_events >= 1);
    CHECK(h.combat.ThreatTop(mob) == std::optional<EntityId>(tank));
    CHECK(h.combat.Stats().threat_updates > 0);
    // 第二个来源也累积（平局取首个，Top 仍为 tank）
    auto dps = h.Spawn(3, 3000);  // Player
    (void)h.combat.Update(h.Ctx(now));
    (void)h.combat.EnterCombat(dps, mob, 1);
    h.Cast(dps, 1001, mob, now);
    CHECK(h.combat.ThreatTop(mob) == std::optional<EntityId>(tank));
    CHECK(h.combat.Stats().threat_updates >= 2);
}

// ===========================================================================
// 4. 嘲讽（Scale ×1.5 置顶，§8）：机制在 ThreatTable，集成验证 Top 切换
// ===========================================================================
void test_taunt() {
    ThreatTable t;
    t.Add(10, 100);
    t.Add(20, 100);
    CHECK(t.Top() == std::optional<EntityId>(10));  // 平局取首个
    t.Scale(20, 1.5);  // 20 → 150，置顶
    CHECK(t.Top() == std::optional<EntityId>(20));
}

// ===========================================================================
// 5. 脱战计时：6 秒无战斗行为 → LeaveCombat，清仇恨
// ===========================================================================
void test_leave_combat_timeout() {
    Harness h;
    auto t0 = core::MonotonicClock::Point();
    auto a = h.Spawn(1, 1000);
    auto b = h.Spawn(2, 1000);
    (void)h.combat.Update(h.Ctx(t0));
    CHECK(h.combat.EnterCombat(a, b, 1).HasValue());
    // 5.9s 后仍在战斗
    (void)h.combat.Update(h.Ctx(t0 + std::chrono::milliseconds(5900)));
    CHECK(h.combat.HasFlag(a, CombatFlag::InCombat));
    // 6.1s 后脱战
    (void)h.combat.Update(h.Ctx(t0 + std::chrono::milliseconds(6100)));
    CHECK(!h.combat.HasFlag(a, CombatFlag::InCombat));
    CHECK(!h.combat.HasFlag(b, CombatFlag::InCombat));
    CHECK(h.combat.Stats().leave_timeout >= 1);
}

// ===========================================================================
// 6. 打断：Interrupt 委托 SkillSystem，发布 SkillInterrupted
// ===========================================================================
void test_interrupt() {
    Harness h;
    auto now = core::MonotonicClock::Point();
    auto a = h.Spawn(1, 2000);
    auto b = h.Spawn(2, 2000, 0, 0, EntityType::Monster);  // 受击方须异类型才能被单体技能选中
    (void)h.combat.Update(h.Ctx(now));
    (void)h.combat.EnterCombat(a, b, 1);
    // 起手一个读条技能（cast_time>0，id 1005 ChanneledBlast）→ 进入 Casting 状态
    std::size_t before = h.skill_interrupted;
    h.Cast(a, 1005, b, now);
    CHECK(h.combat.HasFlag(a, CombatFlag::Casting));  // 读条标志已置
    CHECK(h.combat.Interrupt(a, InterruptReason::Hit, 1).HasValue());
    h.DrainAll();                                       // 派发 SkillInterrupted 给订阅者
    CHECK(h.skill_interrupted == before + 1);          // 委托 SkillSystem 发布 SkillInterrupted
    CHECK(!h.combat.HasFlag(a, CombatFlag::Casting));  // 打断清读条标志
}

// ===========================================================================
// 7. 控制类 Buff 联动：眩晕 → CombatFlag::Stunned（影响移动/施法，§6）
// ===========================================================================
void test_control_buff_links() {
    Harness h;
    auto now = core::MonotonicClock::Point();
    auto a = h.Spawn(1, 2000);
    auto b = h.Spawn(2, 2000);
    (void)h.combat.Update(h.Ctx(now));
    (void)h.combat.EnterCombat(a, b, 1);
    // 给 a 施加眩晕 Buff（id 1004，control=1）
    auto r = h.buffs.Apply(1, 1004, 1, 1, now);
    CHECK(r.HasValue());
    (void)h.combat.Update(h.Ctx(now));  // SyncControlFlags
    CHECK(h.combat.HasFlag(a, CombatFlag::Stunned));
    CHECK(!h.combat.HasFlag(a, CombatFlag::Rooted));
    // 移除眩晕后同步标志清除
    (void)h.buffs.Remove(1, 1004, RemoveReason::Manual, 1);
    (void)h.combat.Update(h.Ctx(now));
    CHECK(!h.combat.HasFlag(a, CombatFlag::Stunned));
}

// ===========================================================================
// 8. 死亡清理：清 Buff / 仇恨 / 标志，只发一次 EntityDied（§19）
// ===========================================================================
void test_death_clears() {
    Harness h;
    auto now = core::MonotonicClock::Point();
    auto a = h.Spawn(1, 5000);
    auto b = h.Spawn(2, 3000);
    (void)h.combat.Update(h.Ctx(now));
    (void)h.combat.EnterCombat(a, b, 1);
    (void)h.combat.EnterCombat(b, a, 1);
    CHECK(h.buffs.Apply(1, 1003, 1, 1, now).HasValue());  // 护盾 Buff
    CHECK(h.buffs.ActiveBuffCount(1) >= 1);
    (void)h.combat.OnDeath(a, b, 1);
    CHECK(!h.combat.HasFlag(a, CombatFlag::InCombat));
    CHECK(h.combat.HasFlag(a, CombatFlag::Dead));
    CHECK(h.combat.ThreatTop(a) == std::nullopt);  // 仇恨清空
    CHECK(h.buffs.ActiveBuffCount(1) == 0);          // Buff 清空
    CHECK(h.combat.Stats().deaths >= 1);
}

// ===========================================================================
// 9. 无效实体：返回 NOT_FOUND，不影响其他实体（§19）
// ===========================================================================
void test_invalid_entity() {
    Harness h;
    auto now = core::MonotonicClock::Point();
    auto a = h.Spawn(1, 1000);
    auto b = h.Spawn(2, 1000);
    (void)h.combat.Update(h.Ctx(now));
    (void)h.combat.EnterCombat(a, b, 1);
    CHECK(!h.combat.EnterCombat(0, b, 1).HasValue());     // 无效
    CHECK(!h.combat.CastSkill(CastRequest{}, h.Ctx(now)).HasValue());  // caster=0
    CHECK(h.combat.HasFlag(a, CombatFlag::InCombat));     // 其他实体不受影响
    CHECK(h.combat.HasFlag(b, CombatFlag::InCombat));
}

// ===========================================================================
// 10. 集成：5 人小队 vs 5 只精英（坦克嘲讽/输出/治疗/DOT/死亡/脱战）
// ===========================================================================
void test_integration_5v5() {
    Harness h;
    auto now = core::MonotonicClock::Point();
    // 5 玩家（坦克 / 3 DPS / 治疗），均为 Player
    std::vector<EntityId> players;
    for (int i = 0; i < 5; ++i) players.push_back(h.Spawn(100 + i, 8000, 200, 100));
    // 5 精英为 Monster（否则玩家单体技能会因同类型被拒，§15 目标类型门）
    std::vector<EntityId> mobs;
    for (int i = 0; i < 5; ++i) mobs.push_back(h.Spawn(200 + i, 4000, 150, 80, EntityType::Monster));
    (void)h.combat.Update(h.Ctx(now));

    // 全员互殴（玩家↔精英交叉类型）
    for (auto p : players) (void)h.combat.EnterCombat(p, mobs[0], 1);
    for (auto m : mobs) (void)h.combat.EnterCombat(m, players[0], 1);

    // 坦克先打 mob[0]，随后 3 名 DPS 各打一次（每位玩家仅一次，避开真实冷却）
    h.Cast(players[0], 1001, mobs[0], now);
    h.Cast(players[1], 1001, mobs[0], now);
    h.Cast(players[2], 1001, mobs[0], now);
    h.Cast(players[3], 1001, mobs[0], now);
    CHECK(h.damage_events >= 4);
    // 仇恨表在 mob[0] 上累积（事件驱动 §8）；Top 为首位贡献者（坦克）
    CHECK(h.combat.ThreatTop(mobs[0]).has_value());
    CHECK(h.combat.ThreatTop(mobs[0]) == std::optional<EntityId>(players[0]));
    CHECK(h.combat.Stats().threat_updates >= 4);

    // 治疗：怪物先打治疗者（怪物→玩家合法），随后治疗者自愈（Self）
    h.Cast(mobs[0], 1001, players[4], now);
    std::int64_t hp_pre = h.roles.Find(104)->hp;  // < 8000
    h.Cast(players[4], 2001, players[4], now);     // HealingTouch（Self）
    CHECK(h.heal_events >= 1);
    CHECK(h.roles.Find(104)->hp > hp_pre);          // 自愈回血（未满时生效）

    // DOT：给 mob[1] 上 Poison（周期伤害），推进时间触发 5 跳
    CHECK(h.buffs.Apply(201, 1005, 1, 1, now).HasValue());
    std::int64_t hp_mob1_before = h.roles.Find(201)->hp;  // 4000
    for (int i = 1; i <= 5; ++i) {
        h.buffs.Tick(h.Ctx(now + std::chrono::milliseconds(1100 * i)));
    }
    CHECK(h.roles.Find(201)->hp < hp_mob1_before);  // DOT 掉血

    // 致死：直接驱动 OnDeath（真实致死由 DamageSystem 判定，此处验证死亡清场）
    std::size_t deaths_before = h.combat.Stats().deaths;
    (void)h.combat.OnDeath(mobs[0], players[1], 1);
    CHECK(!h.combat.HasFlag(mobs[0], CombatFlag::InCombat));
    CHECK(h.combat.ThreatTop(mobs[0]) == std::nullopt);  // 仇恨清空
    CHECK(h.combat.Stats().deaths == deaths_before + 1);

    // 脱战：6s 静默后全员脱战
    (void)h.combat.Update(h.Ctx(now + std::chrono::milliseconds(6100)));
    CHECK(!h.combat.HasFlag(players[0], CombatFlag::InCombat));
    CHECK(!h.combat.HasFlag(mobs[1], CombatFlag::InCombat));
}

}  // namespace

int main() {
    test_threat_table_unit();
    test_combat_flags();
    test_threat_accumulate();
    test_taunt();
    test_leave_combat_timeout();
    test_interrupt();
    test_control_buff_links();
    test_death_clears();
    test_invalid_entity();
    test_integration_5v5();

    if (g_fail == 0) {
        ErrorFmt("Combat.Suite: ALL PASS\n");
        return 0;
    }
    ErrorFmt("Combat.Suite: %d FAIL\n", g_fail);
    return g_fail;
}
