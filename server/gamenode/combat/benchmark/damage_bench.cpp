// server/gamenode/combat/benchmark/damage_bench.cpp — TASK-022 §18 / §22
//
// 输出机器可读的 bench/damage.txt（key=value，供 assert_metric 解析）：
//   compute_damage_ns  —— 单次 ComputeDamage（纯函数，随机量由调用方传入）平均耗时（阈值 ≤ 50ns）
//   apply_damage_ns    —— 单次 ApplyDamage（算 + 改状态 + 发事件 + 统计 + 采样）平均耗时（§22 期望 < 200ns）
//   damage_per_1k_ns   —— 1000 次完整结算的耗时（§22 期望 < 200us = 200000ns）
//   alloc_per_damage   —— 单次结算的堆分配次数（§21 红线：必须 = 0）
//
// 内存口径（复用 TASK-016）：替换全局 operator new/delete 统计**分配次数**增量。
// 之所以统计「次数」而不是「字节数」：热路径红线要求的是"零分配"，
// 字节数会被偶发的一次大分配掩盖，次数才是可断言的硬指标。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf；
// 结果文件 bench/damage.txt 经 std::ofstream 写入（相对 cwd，验收脚本同路径读取）。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

// ---- 全局堆分配计数（必须定义在 operator new/delete 使用之前） ----
namespace {
std::int64_t g_alloc_count = 0;
std::int64_t g_free_count = 0;
}  // namespace

void* operator new(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    ++g_alloc_count;
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
    if (p != nullptr) {
        ++g_free_count;
        std::free(p);
    }
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
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

using Clock = std::chrono::steady_clock;

constexpr SceneId kScene = 1;
constexpr NodeId kOwner = 1;
constexpr const char* kFormulaPath = "config/gameplay/combat/formula.json";

std::int64_t to_ns(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

/// 固定容量 Arena（帧内 bump 分配，Tick 结束 Reset）。
class FixedArena final : public core::Arena {
public:
    explicit FixedArena(std::size_t bytes) : core::Arena(bytes) {}
};

void ForceAttr(Character* c, AttrType t, std::int64_t target) {
    c->attrs.Recompute();
    const std::int64_t cur = c->attrs.Total(t);
    if (cur != target) {
        c->attrs.base[static_cast<std::size_t>(t)] += (target - cur);
        c->attrs.Recompute();
    }
}

ExpCurve MakeCurve() {
    auto c = ExpCurve::FromConfig(ExpCurveConfig{60, 100.0, 1.0});
    if (!c.HasValue()) return ExpCurve::FromConfig(ExpCurveConfig{1, 1.0, 1.0}).Value();
    return c.Value();
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

    explicit Harness(const DamageFormula& f) : dmg(f, roles, mgr, kScene, &bus) {}
    SceneContext Ctx() {
        return SceneContext(kScene, SceneType::World, kOwner, core::MonotonicClock::Point(),
                            tick, mgr, bus, sched, arena);
    }
    EntityId Spawn(CharacterId cid, std::int64_t max_hp, std::int64_t attack,
                   std::int64_t defense, std::int64_t crit_rate, std::int64_t crit_damage) {
        const auto c = roles.LoadOrCreate(static_cast<PlayerId>(cid), cid, Ctx());
        if (!c.HasValue()) return 0;
        Character* cp = roles.Find(cid);
        cp->attrs.base[static_cast<std::size_t>(AttrType::Agility)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Strength)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Intellect)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Stamina)] = 0;
        ForceAttr(cp, AttrType::Attack, attack);
        ForceAttr(cp, AttrType::Defense, defense);
        ForceAttr(cp, AttrType::CritRate, crit_rate);
        ForceAttr(cp, AttrType::CritDamage, crit_damage);
        ForceAttr(cp, AttrType::MaxHp, max_hp);
        cp->hp = max_hp;
        const auto e = mgr.Create(EntityType::Player, kScene, Position{});
        if (!e.HasValue()) return 0;
        const EntityId id = e.Value()->Id();
        (void)roles.AttachToScene(cid, id, kScene);
        dmg.BindAvatar(id, cid);
        return id;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t iterations = 1000000;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--iterations") == 0) {
            iterations = std::strtoull(argv[i + 1], nullptr, 10);
            ++i;
        }
    }

    auto lf = DamageFormula::LoadFromFile(kFormulaPath);
    if (!lf.HasValue()) {
        ErrorFmt("FATAL: cannot load %s: %s\n", kFormulaPath, lf.Err().ToString().c_str());
        return 1;
    }
    const DamageFormula f = lf.Value();
    Harness h(f);

    // 1 个施法者 + 200 个目标（轮流挨打，模拟真实战斗的目标分布）
    constexpr int kTargets = 200;
    constexpr CharacterId kSrcCid = 1;
    // 施法者：Attack 60、暴击率 5%、暴击伤害 1.5x
    const EntityId src = h.Spawn(kSrcCid, 5000000, 60, 0, 500, 15000);
    std::vector<EntityId> targets;
    targets.reserve(kTargets);
    // 目标：Defense 10；MaxHp 取钳制上限内的大值，确保整轮 benchmark 内不会死
    // （死亡后 ApplyDamage 返回 NOT_FOUND，会污染耗时与分配统计）
    for (int i = 0; i < kTargets; ++i) {
        targets.push_back(h.Spawn(static_cast<CharacterId>(1000 + i), 5000000, 0, 10, 0, 0));
    }
    Character* sc = h.roles.Find(kSrcCid);
    Character* tc0 = h.roles.Find(1000);

    // ---- compute_damage_ns：纯函数，随机量预生成（不把 PRNG 成本算进来）----
    constexpr int kRollCount = 1024;
    std::vector<DamageRolls> rolls;
    rolls.reserve(kRollCount);
    {
        Prng p(0);
        p.Seed(kScene, 1, 1);
        for (int i = 0; i < kRollCount; ++i) {
            DamageRolls r{};
            r.crit = p.NextScaled(static_cast<std::uint32_t>(f.rate_scale));
            r.dodge = p.NextScaled(static_cast<std::uint32_t>(f.rate_scale));
            rolls.push_back(r);
        }
    }

    DamageRequest req{};
    req.source = src;
    req.target = targets[0];
    req.school = DamageSchool::Physical;
    req.base_amount = 50;
    req.coefficient = 1.5f;
    req.can_crit = true;
    req.can_be_dodged = true;
    req.trace = 1u;

    // 预热
    std::int64_t sink_sum = 0;
    for (std::uint64_t i = 0; i < 10000; ++i) {
        sink_sum += h.dmg.ComputeDamage(req, sc->attrs, tc0->attrs,
                                       rolls[static_cast<std::size_t>(i % kRollCount)]).final_amount;
    }

    std::vector<double> cd_samples;
    constexpr int kRounds = 9;
    for (int round = 0; round < kRounds; ++round) {
        const auto a = Clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            sink_sum += h.dmg
                            .ComputeDamage(req, sc->attrs, tc0->attrs,
                                           rolls[static_cast<std::size_t>(i % kRollCount)])
                            .final_amount;
        }
        const auto b = Clock::now();
        cd_samples.push_back(static_cast<double>(to_ns(a, b)) /
                             static_cast<double>(iterations));
    }
    // 取中位数（避免被调度抖动带偏）
    std::nth_element(cd_samples.begin(), cd_samples.begin() + kRounds / 2, cd_samples.end());
    const double compute_damage_ns = cd_samples[static_cast<std::size_t>(kRounds / 2)];

    // ---- apply_damage_ns + alloc_per_damage ----
    // 先跑一轮预热（角色/实体容器已 Reserve，运行期不应再扩容）
    for (std::uint64_t i = 0; i < 10000; ++i) {
        req.target = targets[static_cast<std::size_t>(i % kTargets)];
        (void)h.dmg.ApplyDamage(req, h.Ctx());
    }
    while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();

    const std::int64_t alloc_before = g_alloc_count;
    const auto a2 = Clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        req.target = targets[static_cast<std::size_t>(i % kTargets)];
        const auto r = h.dmg.ApplyDamage(req, h.Ctx());
        if (r.HasValue()) sink_sum += r.Value().final_amount;
    }
    const auto b2 = Clock::now();
    const std::int64_t alloc_after = g_alloc_count;
    const double apply_damage_ns =
        static_cast<double>(to_ns(a2, b2)) / static_cast<double>(iterations);
    const double alloc_per_damage =
        static_cast<double>(alloc_after - alloc_before) / static_cast<double>(iterations);

    // ---- damage_per_1k_ns：1000 次完整结算的耗时（§22 期望 < 200us）----
    constexpr std::uint64_t kPer1k = 1000;
    const auto a3 = Clock::now();
    for (std::uint64_t i = 0; i < kPer1k; ++i) {
        req.target = targets[static_cast<std::size_t>(i % kTargets)];
        (void)h.dmg.ApplyDamage(req, h.Ctx());
    }
    const auto b3 = Clock::now();
    const double damage_per_1k_ns = static_cast<double>(to_ns(a3, b3));

    // ---- 输出 ----
    const DamageStats s = h.dmg.Stats();
    {
        std::ofstream out("bench/damage.txt");
        out << "compute_damage_ns=" << compute_damage_ns << "\n";
        out << "apply_damage_ns=" << apply_damage_ns << "\n";
        out << "damage_per_1k_ns=" << damage_per_1k_ns << "\n";
        out << "alloc_per_damage=" << alloc_per_damage << "\n";
    }
    LineFmt("iterations=%llu targets=%d\n", static_cast<unsigned long long>(iterations),
            kTargets);
    LineFmt("compute_damage_ns=%.3f (le 50)\n", compute_damage_ns);
    LineFmt("apply_damage_ns=%.3f (expect < 200)\n", apply_damage_ns);
    LineFmt("damage_per_1k_ns=%.0f (expect < 200000)\n", damage_per_1k_ns);
    LineFmt("alloc_per_damage=%.3f (le 0)\n", alloc_per_damage);
    LineFmt("stats: events=%llu crit_bp=%lld dodge_bp=%lld avg_final=%lld log_records=%llu\n",
            static_cast<unsigned long long>(s.damage_events),
            static_cast<long long>(s.CritRateBp()), static_cast<long long>(s.DodgeRateBp()),
            static_cast<long long>(s.AvgDamageX10000()),
            static_cast<unsigned long long>(s.log_records));
    LineFmt("(sink_sum=%lld)\n", static_cast<long long>(sink_sum));
    return 0;
}
