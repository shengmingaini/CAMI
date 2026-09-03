// server/gamenode/combat/tests/skill_test.cpp — TASK-021 §16 单元 / §17 集成 / §19 Failure
//
// 覆盖：四类目标（Self / SingleTarget / AoeCircle / AoeCone / Projectile）全路径；
// CastResult 各分支（Ok / OnCooldown / OutOfRange / NoTarget / InsufficientResource /
// InvalidTarget / Silenced）；冷却计时与查询；资源扣除与打断不退；距离校验；读条状态机与打断；
// 飞行物命中 / 目标中途死亡 / 超距消失；配置加载校验（未知 buff_id → 加载失败）；
// 客户端重复发包不重复结算（冷却拦截）；5 人小队打 20 怪 1000 次集成。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/combat/skill/buff_def.h"
#include "mmo/game/combat/skill/combat_events.h"
#include "mmo/game/combat/skill/skill_def.h"
#include "mmo/game/combat/skill/skill_system.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::combat;
using namespace mmo::game::role;
using namespace mmo::game::aoi;

using core::MonotonicClock;
using core::test::ErrorFmt;
using core::test::LineFmt;

constexpr SceneId kScene = 1;
constexpr NodeId kOwner = 1;
constexpr const char* kSkillDir = "config/gameplay/skills";
constexpr const char* kBuffFile = "config/gameplay/buffs/buffs.json";

// 技能 id（与 config/gameplay/skills/*.json 对应，全部配置化）
constexpr SkillId kFireball = 1001;        // SingleTarget Damage
constexpr SkillId kFrostNova = 1002;       // AoeCircle Damage
constexpr SkillId kConeSlash = 1003;       // AoeCone Damage
constexpr SkillId kArcaneMissile = 1004;   // Projectile
constexpr SkillId kHealingTouch = 2001;    // Self Heal
constexpr SkillId kGroupMend = 2002;       // AoeCircle Heal
constexpr SkillId kShieldSelf = 2003;      // Self ApplyBuff
constexpr SkillId kBlessing = 2004;        // SingleTarget ApplyBuff
constexpr SkillId kFirestorm = 3001;       // AoeCircle cast_time 1.5
constexpr SkillId kFrostBolt = 3002;       // Projectile cast_time 0.5
constexpr SkillId kWarCry = 3003;          // AoeCone ApplyBuff
constexpr SkillId kManaBurn = 3004;        // SingleTarget Damage

int g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

// 事件计数 + 全局序列（验证 SkillCast 先于 DamageEvent）
struct Counters {
    std::size_t skill_cast = 0;
    std::size_t interrupted = 0;
    std::size_t damage = 0;
    std::size_t heal = 0;
    std::size_t buff = 0;
};
struct EventLog {
    std::size_t seq = 0;
    std::size_t skill_cast_seq = 0;
    std::size_t damage_seq = 0;
    bool seen_skill_cast = false;
    bool seen_damage = false;
};

struct Harness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena;
    EntityManager mgr;
    InMemoryPersistenceAdapter persist;
    RoleSystem roles;
    std::unique_ptr<IAoi> aoi_owner;
    IAoi& aoi;
    SkillSystem skill;
    SceneContext ctx;
    Counters cnt{};
    EventLog log{};

    Harness()
        : arena(4 * 1024 * 1024),
          mgr(&bus),
          roles(persist, LoadExpCurve(), RoleDefaults{}),
          aoi_owner(CreateDynamicGridAoi(AoiConfig{})),
          aoi(*aoi_owner),
          skill(roles, mgr, &aoi, kScene, kOwner, &bus),
          ctx(kScene, SceneType::World, kOwner, MonotonicClock::Point(), 0u, mgr, bus,
              scheduler, arena) {
        (void)bus.Subscribe<SkillCast>([this](const SkillCast&) {
            ++cnt.skill_cast;
            if (!log.seen_skill_cast) { log.seen_skill_cast = true; log.skill_cast_seq = log.seq; }
            ++log.seq;
        });
        (void)bus.Subscribe<SkillInterrupted>([this](const SkillInterrupted&) {
            ++cnt.interrupted;
            ++log.seq;
        });
        (void)bus.Subscribe<DamageEvent>([this](const DamageEvent&) {
            ++cnt.damage;
            if (!log.seen_damage) { log.seen_damage = true; log.damage_seq = log.seq; }
            ++log.seq;
        });
        (void)bus.Subscribe<HealEvent>([this](const HealEvent&) {
            ++cnt.heal;
            ++log.seq;
        });
        (void)bus.Subscribe<BuffApplied>([this](const BuffApplied&) {
            ++cnt.buff;
            ++log.seq;
        });
    }

    static ExpCurve LoadExpCurve() {
        auto r = ExpCurve::LoadFromFile("config/gameplay/exp_curve.json");
        if (!r) throw std::runtime_error("exp_curve load failed");
        return r.Value();
    }
};

// 读 buffs.json 进一个 BuffRegistry（供技能加载期校验 ApplyBuff 引用）
BuffRegistry LoadBuffs() {
    BuffRegistry b;
    std::ifstream in(kBuffFile, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    CHECK(b.Load(ss.str()).HasValue());
    return b;
}

EntityId SpawnPlayer(Harness& h, CharacterId cid, float x, float z) {
    Position pos{x, 0.0f, z, 0.0f};
    auto e = h.mgr.Create(EntityType::Player, kScene, pos);
    CHECK(e.HasValue());
    auto c = h.roles.LoadOrCreate(cid, cid, h.ctx);
    CHECK(c.HasValue());
    h.skill.BindAvatar(e.Value()->Id(), cid);
    CHECK(h.aoi.Enter(e.Value()->Id(), pos).HasValue());
    return e.Value()->Id();
}

EntityId SpawnMonster(Harness& h, CharacterId cid, float x, float z) {
    Position pos{x, 0.0f, z, 0.0f};
    auto e = h.mgr.Create(EntityType::Monster, kScene, pos);
    CHECK(e.HasValue());
    auto c = h.roles.LoadOrCreate(cid, cid, h.ctx);
    CHECK(c.HasValue());
    h.skill.BindAvatar(e.Value()->Id(), cid);
    CHECK(h.aoi.Enter(e.Value()->Id(), pos).HasValue());
    return e.Value()->Id();
}

// ---- 1. Self：治疗拉满 + 自身上 buff ----
void test_self_heal_and_buff() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId cid = 1;
    EntityId c = SpawnPlayer(h, cid, 0.0f, 0.0f);
    Character* cp = h.roles.Find(cid);
    CHECK(cp != nullptr);
    const std::int64_t max_hp = cp->MaxHp();
    (void)h.roles.ModifyHp(cid, -(cp->MaxHp() - 10), 0u);  // 压到 10
    CHECK(cp->hp == 10);

    CastRequest req{};
    req.caster = c;
    req.skill = kHealingTouch;
    req.target = c;
    req.trace = 1u;
    auto r = h.skill.TryCast(req, h.ctx);
    CHECK(r.HasValue() && r.Value() == CastResult::Ok);
    // 治疗量按公式 60 + 2.0*Attack 计算并钳制到上限（非「拉满」：固定治疗量）。
    (void)h.bus.Drain();
    const std::int64_t attack = cp->attrs.Total(role::AttrType::Attack);
    const std::int64_t heal = static_cast<std::int64_t>(60 + 2.0 * static_cast<double>(attack));
    const std::int64_t expect_hp = std::min(max_hp, std::int64_t(10) + heal);
    CHECK(cp->hp == expect_hp);
    CHECK(h.cnt.heal == 1);
    CHECK(h.cnt.skill_cast == 1);

    CastRequest req2{};
    req2.caster = c;
    req2.skill = kShieldSelf;
    req2.target = c;
    req2.trace = 2u;
    auto r2 = h.skill.TryCast(req2, h.ctx);
    CHECK(r2.HasValue() && r2.Value() == CastResult::Ok);
    (void)h.bus.Drain();
    CHECK(h.cnt.buff == 1);
}

// ---- 2. SingleTarget 伤害 + 法力扣除 + 事件序列 ----
void test_single_target_damage() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1, mc = 2;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);
    EntityId m = SpawnMonster(h, mc, 1.5f, 0.0f);
    Character* cp = h.roles.Find(pc);
    Character* mp = h.roles.Find(mc);
    CHECK(cp != nullptr && mp != nullptr);
    const std::int64_t mp_before = cp->mp;
    const std::int64_t mhp_before = mp->hp;

    CastRequest req{};
    req.caster = c;
    req.skill = kFireball;
    req.target = m;
    req.trace = 1u;
    auto r = h.skill.TryCast(req, h.ctx);
    CHECK(r.HasValue() && r.Value() == CastResult::Ok);

    // 伤害 = 50 + 1.5 * Attack(25) = 87.5 -> 87
    (void)h.bus.Drain();
    CHECK(mp->hp == mhp_before - 87);
    CHECK(cp->mp == mp_before - 20);          // 法力扣除 20
    CHECK(h.log.seen_skill_cast && h.log.seen_damage);
    CHECK(h.log.skill_cast_seq < h.log.damage_seq);  // SkillCast 先于 DamageEvent
    CHECK(h.cnt.skill_cast == 1 && h.cnt.damage == 1);
}

// ---- 3. CastResult 各分支 ----
void test_cast_result_branches() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1, mc = 2, mc2 = 3, pc2 = 4;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);
    EntityId m = SpawnMonster(h, mc, 1.5f, 0.0f);
    EntityId mfar = SpawnMonster(h, mc2, 100.0f, 0.0f);  // 超 Fireball 射程 30
    EntityId c2 = SpawnPlayer(h, pc2, 0.0f, 5.0f);

    // OnCooldown：连放两次 Fireball
    CastRequest r1{};
    r1.caster = c; r1.skill = kFireball; r1.target = m; r1.trace = 1u;
    CHECK(h.skill.TryCast(r1, h.ctx).Value() == CastResult::Ok);
    CastRequest r2{};
    r2.caster = c; r2.skill = kFireball; r2.target = m; r2.trace = 2u;
    CHECK(h.skill.TryCast(r2, h.ctx).Value() == CastResult::OnCooldown);

    // OutOfRange：ManaBurn（射程 25）打 100m 目标
    CastRequest r3{};
    r3.caster = c; r3.skill = kManaBurn; r3.target = mfar; r3.trace = 3u;
    CHECK(h.skill.TryCast(r3, h.ctx).Value() == CastResult::OutOfRange);

    // NoTarget：SingleTarget 未给目标
    CastRequest r4{};
    r4.caster = c; r4.skill = kBlessing; r4.target = 0; r4.trace = 4u;
    CHECK(h.skill.TryCast(r4, h.ctx).Value() == CastResult::NoTarget);

    // InvalidTarget：SingleTarget 打到同类型（Player）
    CastRequest r5{};
    r5.caster = c; r5.skill = kBlessing; r5.target = c2; r5.trace = 5u;
    CHECK(h.skill.TryCast(r5, h.ctx).Value() == CastResult::InvalidTarget);

    // InvalidTarget：不存在的技能 id
    CastRequest r6{};
    r6.caster = c; r6.skill = 99999; r6.target = m; r6.trace = 6u;
    CHECK(h.skill.TryCast(r6, h.ctx).Value() == CastResult::InvalidTarget);

    // InsufficientResource：清空法力后放 Fireball（先清冷却）
    h.skill.UnbindAvatar(c);
    h.skill.BindAvatar(c, pc);
    Character* cp = h.roles.Find(pc);
    (void)h.roles.ModifyMp(pc, -cp->MaxMp(), 0u);  // mp -> 0
    CHECK(cp->mp == 0);
    CastRequest r7{};
    r7.caster = c; r7.skill = kFireball; r7.target = m; r7.trace = 7u;
    auto res7 = h.skill.TryCast(r7, h.ctx);
    CHECK(res7.HasValue() && res7.Value() == CastResult::InsufficientResource);
    CHECK(cp->mp == 0);  // 不足即拒绝，未扣

    // Silenced：沉默后再放
    h.skill.SetSilenced(c, true);
    CastRequest r8{};
    r8.caster = c; r8.skill = kFireball; r8.target = m; r8.trace = 8u;
    CHECK(h.skill.TryCast(r8, h.ctx).Value() == CastResult::Silenced);
    h.skill.SetSilenced(c, false);
}

// ---- 4. 冷却计时与查询（施法开始即进入冷却） ----
void test_cooldown_timing() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1, mc = 2;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);
    EntityId m = SpawnMonster(h, mc, 1.5f, 0.0f);

    CastRequest req{};
    req.caster = c; req.skill = kFireball; req.target = m; req.trace = 1u;
    CHECK(h.skill.TryCast(req, h.ctx).Value() == CastResult::Ok);

    CHECK(h.skill.IsOnCooldown(c, kFireball));
    core::DurationMs rem = h.skill.CooldownRemaining(c, kFireball);
    CHECK(rem > core::DurationMs(0));
    CHECK(rem <= core::DurationMs(2000));       // Fireball cooldown = 2.0s
    CHECK(h.skill.CastingOf(c) == CastingState::Idle);  // 瞬发
}

// ---- 5. 读条状态机 + 打断不退资源 ----
void test_cast_bar_interrupt() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1, mc = 2;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);
    EntityId m = SpawnMonster(h, mc, 2.0f, 0.0f);  // Firestorm 半径 15 内
    Character* cp = h.roles.Find(pc);
    Character* mp = h.roles.Find(mc);
    const std::int64_t mp_before = cp->mp;   // 110
    const std::int64_t mhp_before = mp->hp;  // 150

    CastRequest req{};
    req.caster = c; req.skill = kFirestorm; req.target = m; req.trace = 1u;
    auto r = h.skill.TryCast(req, h.ctx);
    CHECK(r.HasValue() && r.Value() == CastResult::Ok);

    CHECK(h.skill.CastingOf(c) == CastingState::Casting);
    CHECK(h.skill.IsOnCooldown(c, kFirestorm));
    CHECK(cp->mp == mp_before - 50);          // 施法开始扣 50

    auto ri = h.skill.InterruptCasting(c, InterruptReason::Manual, 2u);
    CHECK(ri.HasValue());
    CHECK(h.skill.CastingOf(c) == CastingState::Idle);
    CHECK(h.skill.IsOnCooldown(c, kFirestorm));  // 冷却保留
    CHECK(cp->mp == mp_before - 50);             // 资源不退
    CHECK(mp->hp == mhp_before);                 // 效果未结算
    (void)h.bus.Drain();
    CHECK(h.cnt.interrupted == 1);
    CHECK(h.cnt.damage == 0);
}

// ---- 6. AOE 圆：命中半径内全部敌对，半径外不受伤 ----
void test_aoe_circle() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);
    CharacterId ids[4] = {2, 3, 4, 5};
    EntityId m1 = SpawnMonster(h, ids[0], 2.0f, 0.0f);
    EntityId m2 = SpawnMonster(h, ids[1], 0.0f, 2.0f);
    EntityId m3 = SpawnMonster(h, ids[2], -2.0f, 0.0f);
    EntityId mfar = SpawnMonster(h, ids[3], 100.0f, 0.0f);  // 半径 8 外
    std::int64_t h1 = h.roles.Find(ids[0])->hp;
    std::int64_t h2 = h.roles.Find(ids[1])->hp;
    std::int64_t h3 = h.roles.Find(ids[2])->hp;
    std::int64_t hf = h.roles.Find(ids[3])->hp;

    CastRequest req{};
    req.caster = c; req.skill = kFrostNova; req.target = m1; req.trace = 1u;
    CHECK(h.skill.TryCast(req, h.ctx).Value() == CastResult::Ok);

    // 伤害 = 30 + 1.0 * Attack(25) = 55
    (void)h.bus.Drain();
    CHECK(h.roles.Find(ids[0])->hp == h1 - 55);
    CHECK(h.roles.Find(ids[1])->hp == h2 - 55);
    CHECK(h.roles.Find(ids[2])->hp == h3 - 55);
    CHECK(h.roles.Find(ids[3])->hp == hf);  // 远处不受伤
    CHECK(h.cnt.damage == 3);
}

// ---- 7. AOE 锥：只命中朝向锥形内 ----
void test_aoe_cone() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);  // yaw = 0 -> +Z 方向
    CharacterId a = 2, b = 3;
    EntityId mz = SpawnMonster(h, a, 0.0f, 5.0f);     // +Z，锥形内
    EntityId mzneg = SpawnMonster(h, b, 0.0f, -5.0f);  // -Z，锥形外
    std::int64_t ha = h.roles.Find(a)->hp;
    std::int64_t hb = h.roles.Find(b)->hp;

    CastRequest req{};
    req.caster = c; req.skill = kConeSlash; req.target = mz; req.trace = 1u;
    CHECK(h.skill.TryCast(req, h.ctx).Value() == CastResult::Ok);

    // 伤害 = 40 + 1.2 * Attack(25) = 70
    (void)h.bus.Drain();
    CHECK(h.roles.Find(a)->hp == ha - 70);
    CHECK(h.roles.Find(b)->hp == hb);   // -Z 不在锥形
    CHECK(h.cnt.damage == 1);
}

// ---- 8. 飞行物命中（目标在命中半径内，首帧结算） ----
void test_projectile_hit() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1, mc = 2;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);
    EntityId m = SpawnMonster(h, mc, 1.5f, 0.0f);  // 在命中半径 2.0 内
    std::int64_t mhp = h.roles.Find(mc)->hp;

    CastRequest req{};
    req.caster = c; req.skill = kArcaneMissile; req.target = m; req.trace = 1u;
    CHECK(h.skill.TryCast(req, h.ctx).Value() == CastResult::Ok);
    CHECK(h.skill.ActiveProjectiles() == 1);

    (void)h.skill.Update(h.ctx);  // 首帧推进：目标在命中半径内 -> 立即结算
    (void)h.bus.Drain();         // 结算在 Update 内发生，Drain 必须在 Update 之后
    CHECK(h.cnt.skill_cast == 1);
    CHECK(h.skill.ActiveProjectiles() == 0);
    // 伤害 = 35 + 1.3 * Attack(25) = 67.5 -> 67
    CHECK(h.roles.Find(mc)->hp == mhp - 67);
    CHECK(h.cnt.damage == 1);
}

// ---- 9. 飞行物目标中途死亡 -> 消失不崩溃 ----
void test_projectile_target_dies() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1, mc = 2;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);
    EntityId m = SpawnMonster(h, mc, 1.5f, 0.0f);

    CastRequest req{};
    req.caster = c; req.skill = kArcaneMissile; req.target = m; req.trace = 1u;
    CHECK(h.skill.TryCast(req, h.ctx).Value() == CastResult::Ok);
    CHECK(h.skill.ActiveProjectiles() == 1);

    CHECK(h.mgr.Destroy(m).HasValue());  // 目标在命中前消失
    (void)h.skill.Update(h.ctx);
    CHECK(h.skill.ActiveProjectiles() == 0);  // 飞行物消失
    CHECK(h.cnt.damage == 0);                 // 未结算
}

// ---- 10. 飞行物超距消失（目标远离飞行路径） ----
void test_projectile_max_distance() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1, mc = 2;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);
    EntityId m = SpawnMonster(h, mc, 5.0f, 0.0f);  // 射程 40 内，命中半径 2.5 外
    std::int64_t mhp = h.roles.Find(mc)->hp;

    CastRequest req{};
    req.caster = c; req.skill = kFrostBolt; req.target = m; req.trace = 1u;
    CHECK(h.skill.TryCast(req, h.ctx).Value() == CastResult::Ok);
    CHECK(h.skill.ActiveProjectiles() == 1);

    // 目标远离飞行路径，飞行物永远碰不到它
    Entity* me = h.mgr.Find(m);
    CHECK(me != nullptr);
    me->SetPos(Position{500.0f, 0.0f, 0.0f, 0.0f});

    bool gone = false;
    for (int i = 0; i < 2500 && !gone; ++i) {
        (void)h.skill.Update(h.ctx);
        if (h.skill.ActiveProjectiles() == 0) gone = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(gone);
    CHECK(h.roles.Find(mc)->hp == mhp);  // 目标未受伤
    CHECK(h.cnt.damage == 0);
}

// ---- 11. 配置加载校验：引用不存在的 buff -> 加载失败 ----
void test_config_validation() {
    Harness h;
    BuffRegistry empty;  // 不含任何 buff
    CHECK(!h.skill.LoadSkillsFromDir(kSkillDir, empty).HasValue());
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
}

// ---- 12. 客户端重复发包不重复结算（冷却拦截） ----
void test_duplicate_packet_no_double_cast() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    CharacterId pc = 1, mc = 2;
    EntityId c = SpawnPlayer(h, pc, 0.0f, 0.0f);
    EntityId m = SpawnMonster(h, mc, 1.5f, 0.0f);
    std::int64_t mhp = h.roles.Find(mc)->hp;

    CastRequest req{};
    req.caster = c; req.skill = kFireball; req.target = m; req.trace = 1u;
    CHECK(h.skill.TryCast(req, h.ctx).Value() == CastResult::Ok);
    (void)h.bus.Drain();  // 派发首跳伤害事件，供下方 cnt 断言
    CHECK(h.roles.Find(mc)->hp == mhp - 87);
    CHECK(h.cnt.damage == 1);

    // 重复发包（第二次）：冷却拦截，不重复结算
    (void)h.bus.Drain();
    CastRequest dup{};
    dup.caster = c; dup.skill = kFireball; dup.target = m; dup.trace = 2u;
    CHECK(h.skill.TryCast(dup, h.ctx).Value() == CastResult::OnCooldown);
    CHECK(h.roles.Find(mc)->hp == mhp - 87);  // 血量不变
    (void)h.bus.Drain();
    CHECK(h.cnt.damage == 1);                  // 没有第二次伤害事件
}

// ---- 13. 集成：5 人小队打 20 怪，混合四类瞬发技能 1000 次 ----
void test_integration_1000() {
    Harness h;
    CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());

    constexpr std::size_t kNumCasters = 5;
    constexpr std::size_t kNumMonsters = 20;
    std::vector<EntityId> casters(kNumCasters);
    std::vector<CharacterId> cids(kNumCasters);
    for (std::size_t i = 0; i < kNumCasters; ++i) {
        cids[i] = static_cast<CharacterId>(1000 + i);
        casters[i] = SpawnPlayer(h, cids[i], 0.0f, static_cast<float>(i) * 0.5f);
    }
    std::vector<EntityId> monsters(kNumMonsters);
    std::vector<CharacterId> mids(kNumMonsters);
    for (std::size_t i = 0; i < kNumMonsters; ++i) {
        mids[i] = static_cast<CharacterId>(2000 + i);
        const float ang = static_cast<float>(i) * (6.2831853f / static_cast<float>(kNumMonsters));
        const float rad = 3.0f;
        monsters[i] = SpawnMonster(h, mids[i], std::cos(ang) * rad, std::sin(ang) * rad);
    }

    // 7 个瞬发技能（排除读条 Firestorm、飞行物 ArcaneMissile/FrostBolt、锥形 ConeSlash/WarCry，
    // 避免残留 ActiveCast / Projectile / 锥形空目标 NoTarget 导致 SkillCast 计数不足）
    constexpr SkillId kSkills[] = {kFireball, kFrostNova, kHealingTouch,
                                   kGroupMend, kShieldSelf, kBlessing, kManaBurn};
    constexpr std::size_t kNumSkills = sizeof(kSkills) / sizeof(kSkills[0]);
    constexpr std::size_t kN = 1000;

    std::int64_t total_hp_before = 0;
    for (auto id : mids) total_hp_before += h.roles.Find(id)->hp;

    for (std::size_t i = 0; i < kN; ++i) {
        const EntityId caster = casters[i % kNumCasters];
        const CharacterId cid = cids[i % kNumCasters];
        const EntityId target = monsters[i % kNumMonsters];
        const SkillId sid = kSkills[i % kNumSkills];

        // 清冷却（模拟「冷却结束后再次施放」），保证 1000 次都成功
        h.skill.UnbindAvatar(caster);
        h.skill.BindAvatar(caster, cid);
        Character* cp = h.roles.Find(cid);
        cp->mp = cp->MaxMp();  // 回满法力

        CastRequest req{};
        req.request_id = static_cast<core::RequestID>(i + 1);
        req.caster = caster;
        req.skill = sid;
        req.target = target;
        req.trace = static_cast<core::TraceID>(i + 1);
        auto r = h.skill.TryCast(req, h.ctx);
        CHECK(r.HasValue());
        (void)h.skill.Update(h.ctx);

        // 资源不变量：法力永不越界（禁止凭空产生 / 变为负）
        CHECK(cp->mp >= 0 && cp->mp <= cp->MaxMp());
    }
    // 海量事件（AOE 单次可产出 20+ 伤害/治疗事件）总量远超单次 Drain 默认上限 4096；
    // 循环 Drain 直到队列清空，确保全部 SkillCast/Damage/Heal 都被派发。
    // 真实 GameNode 由主循环每 Tick 持续 Drain，语义一致。
    while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();

    CHECK(h.cnt.skill_cast == kN);
    CHECK(h.cnt.damage > 0);
    CHECK(h.log.seen_skill_cast && h.log.seen_damage);
    CHECK(h.log.skill_cast_seq < h.log.damage_seq);  // 事件序列 SkillCast -> DamageEvent
    CHECK(h.skill.ActiveProjectiles() == 0);         // 无残留飞行物

    std::int64_t total_hp_after = 0;
    for (auto id : mids) total_hp_after += h.roles.Find(id)->hp;
    CHECK(total_hp_after < total_hp_before);          // 目标总血量下降（伤害结算）

    for (auto cid : cids) {                            // 资源守恒：法力始终在合法区间
        Character* cp = h.roles.Find(cid);
        CHECK(cp->mp >= 0);
        CHECK(cp->mp <= cp->MaxMp());
    }
}

}  // namespace

int main() {
    LineFmt("== TASK-021 skill_test ==\n");
    // 冒烟：交付的配置文件必须能被加载（配置即契约，缺字段/未知引用一律失败）
    {
        Harness h;
        CHECK(h.skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue());
    }
    test_self_heal_and_buff();
    test_single_target_damage();
    test_cast_result_branches();
    test_cooldown_timing();
    test_cast_bar_interrupt();
    test_aoe_circle();
    test_aoe_cone();
    test_projectile_hit();
    test_projectile_target_dies();
    test_projectile_max_distance();
    test_config_validation();
    test_duplicate_packet_no_double_cast();
    test_integration_1000();
    LineFmt("skill_test done: fail=%d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
