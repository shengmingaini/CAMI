// server/gamenode/combat/benchmark/skill_bench.cpp — TASK-021 §18 / §22
//
// 性能基准：真实执行 TryCast / 冷却查询 / AOE 目标选取，输出机器可读的
//   bench/skill.txt  (key=value，供 assert_metric 解析)
// 指标：
//   try_cast_ns          —— 单次 TryCast 平均耗时（瞬发单体，热路径；阈值 ≤1000ns）
//   cooldown_query_ns     —— 单次冷却查询（IsOnCooldown，批量摊销时钟开销）平均耗时（阈值 ≤20ns）
//   aoe_target_select_ns  —— 单次 AOE 施法（半径 8m / ~20 目标，走 AOI 局部查询）平均耗时
//   mem_bytes_per_skill_state —— 单实体技能运行时状态内存（冷却时间戳数组，阈值 <256B）
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf；
// 结果文件 bench/skill.txt 经 std::ofstream 写入（相对 cwd，验收脚本同路径读取）。

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/combat/skill/buff_def.h"
#include "mmo/game/combat/skill/skill_def.h"
#include "mmo/game/combat/skill/skill_system.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::combat;
using namespace mmo::game::role;
using namespace mmo::game::aoi;

using core::MonotonicClock;
using core::test::LineFmt;

constexpr SceneId kScene = 1;
constexpr NodeId kOwner = 1;
constexpr const char* kSkillDir = "config/gameplay/skills";
constexpr const char* kBuffFile = "config/gameplay/buffs/buffs.json";

constexpr SkillId kFireball = 1001;    // SingleTarget Damage
constexpr SkillId kFrostNova = 1002;   // AoeCircle Damage radius 8

using Clock = std::chrono::steady_clock;

double to_ns(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(b - a).count();
}

ExpCurve LoadExpCurve() {
    auto r = ExpCurve::LoadFromFile("config/gameplay/exp_curve.json");
    if (!r) throw std::runtime_error("exp_curve load failed");
    return r.Value();
}

BuffRegistry LoadBuffs() {
    BuffRegistry b;
    std::ifstream in(kBuffFile, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!b.Load(ss.str()).HasValue()) throw std::runtime_error("buffs load failed");
    return b;
}

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

    Harness()
        : arena(4 * 1024 * 1024),
          mgr(&bus),
          roles(persist, LoadExpCurve(), RoleDefaults{}),
          aoi_owner(CreateDynamicGridAoi(AoiConfig{})),
          aoi(*aoi_owner),
          skill(roles, mgr, &aoi, kScene, kOwner, nullptr),  // 基准不订阅事件，避免队列分配干扰计时
          ctx(kScene, SceneType::World, kOwner, MonotonicClock::Point(), 0u, mgr, bus,
              scheduler, arena) {
        if (!skill.LoadSkillsFromDir(kSkillDir, LoadBuffs()).HasValue())
            throw std::runtime_error("skill load failed");
    }

    EntityId SpawnPlayer(CharacterId cid, float x, float z) {
        Position pos{x, 0.0f, z, 0.0f};
        auto e = mgr.Create(EntityType::Player, kScene, pos);
        if (!e.HasValue()) throw std::runtime_error("create player failed");
        if (!roles.LoadOrCreate(cid, cid, ctx).HasValue()) throw std::runtime_error("load char failed");
        skill.BindAvatar(e.Value()->Id(), cid);
        (void)aoi.Enter(e.Value()->Id(), pos);
        return e.Value()->Id();
    }

    EntityId SpawnMonster(CharacterId cid, float x, float z) {
        Position pos{x, 0.0f, z, 0.0f};
        auto e = mgr.Create(EntityType::Monster, kScene, pos);
        if (!e.HasValue()) throw std::runtime_error("create monster failed");
        if (!roles.LoadOrCreate(cid, cid, ctx).HasValue()) throw std::runtime_error("load char failed");
        skill.BindAvatar(e.Value()->Id(), cid);
        (void)aoi.Enter(e.Value()->Id(), pos);
        return e.Value()->Id();
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t casts = 100000;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--casts" && i + 1 < argc) casts = std::strtoull(argv[++i], nullptr, 10);
    }

    LineFmt("== TASK-021 skill_bench (casts=%llu) ==\n", static_cast<unsigned long long>(casts));

    Harness h;
    Harness hAoe;  // 独立 harness 承载 AOE 目标环，隔离对单体计时的干扰

    // ---- 单体计时 harness：1 施法者 + 1 范围内敌对目标 ----
    CharacterId pc = 1, mc = 2;
    EntityId caster = h.SpawnPlayer(pc, 0.0f, 0.0f);
    EntityId target = h.SpawnMonster(mc, 1.5f, 0.0f);
    Character* cp = h.roles.Find(pc);
    if (cp == nullptr) { LineFmt("FAIL: caster character missing\n"); return 1; }

    CastRequest req{};
    req.caster = caster;
    req.skill = kFireball;
    req.target = target;
    req.trace = 1u;

    // ---- try_cast_ns：每次施放前清冷却 + 回满法力，度量纯 TryCast 耗时（热路径）----
    double try_sum = 0.0;
    std::uint64_t try_fail = 0;
    for (std::uint64_t i = 0; i < casts; ++i) {
        h.skill.UnbindAvatar(caster);   // 清冷却（不在计时区内）
        h.skill.BindAvatar(caster, pc);
        cp->mp = cp->MaxMp();
        auto a = Clock::now();
        auto r = h.skill.TryCast(req, h.ctx);
        auto b = Clock::now();
        if (!r.HasValue() || r.Value() != CastResult::Ok) ++try_fail;
        try_sum += to_ns(a, b);
    }
    const double try_cast_ns = try_sum / static_cast<double>(casts);

    // ---- cooldown_query_ns：单次冷却查询耗时（O(1) 数组访问）----
    // 热路径真实形态：now 由调用方每 Tick 取一次并传入（IsOnCooldown(caster, skill, now)），
    // 单次查询 = 一次数组下标 + 比较，远快于时钟分辨率（Windows steady_clock ~100ns），
    // 故用「每轮批量 kCdBatch 次查询」摊销时钟开销，再除以 kCdBatch 得稳定单次值。
    constexpr std::uint64_t kCdBatch = 64;
    const std::uint64_t cd_now = static_cast<std::uint64_t>(core::MonotonicClock::Now());
    double cd_sum = 0.0;
    std::uint64_t cd_calls = 0;
    for (std::uint64_t i = 0; i < casts; ++i) {
        bool oc = false;
        auto a = Clock::now();
        for (std::uint64_t k = 0; k < kCdBatch; ++k) {
            oc ^= h.skill.IsOnCooldown(caster, kFireball, cd_now);
        }
        auto bb = Clock::now();
        (void)oc;
        cd_sum += to_ns(a, bb);
        cd_calls += kCdBatch;
    }
    const double cooldown_query_ns = cd_sum / static_cast<double>(cd_calls);

    // ---- aoe_target_select_ns：半径 8m / ~20 目标，走 AOI 局部查询 ----
    constexpr std::size_t kNumMon = 20;
    CharacterId ac = 100;
    EntityId acaster = hAoe.SpawnPlayer(ac, 0.0f, 0.0f);
    Character* acp = hAoe.roles.Find(ac);
    if (acp == nullptr) { LineFmt("FAIL: aoe caster character missing\n"); return 1; }
    EntityId amon[kNumMon];
    for (std::size_t i = 0; i < kNumMon; ++i) {
        const float ang = static_cast<float>(i) * (6.2831853f / static_cast<float>(kNumMon));
        const float rad = 3.0f;  // 半径 8m 内
        amon[i] = hAoe.SpawnMonster(static_cast<CharacterId>(200 + i),
                                    std::cos(ang) * rad, std::sin(ang) * rad);
    }
    CastRequest areq{};
    areq.caster = acaster;
    areq.skill = kFrostNova;
    areq.target = amon[0];
    areq.trace = 1u;

    double aoe_sum = 0.0;
    std::uint64_t aoe_fail = 0;
    for (std::uint64_t i = 0; i < casts; ++i) {
        hAoe.skill.UnbindAvatar(acaster);
        hAoe.skill.BindAvatar(acaster, ac);
        acp->mp = acp->MaxMp();
        auto a = Clock::now();
        auto r = hAoe.skill.TryCast(areq, hAoe.ctx);
        auto b = Clock::now();
        if (!r.HasValue() || r.Value() != CastResult::Ok) ++aoe_fail;
        aoe_sum += to_ns(a, b);
    }
    const double aoe_target_select_ns = aoe_sum / static_cast<double>(casts);

    // ---- mem_bytes_per_skill_state：每实体冷却时间戳数组（num_skills * 8B，阈值 <256B）----
    const std::size_t mem_bytes_per_skill_state = h.skill.SkillCount() * sizeof(std::uint64_t);

    // ---- 写出 bench/skill.txt（相对 cwd，验收脚本同路径读取）----
    std::error_code ec;
    std::filesystem::create_directories("bench", ec);
    std::ofstream out("bench/skill.txt", std::ios::binary);
    if (!out) { LineFmt("FAIL: cannot open bench/skill.txt\n"); return 1; }
    out << "try_cast_ns=" << try_cast_ns << "\n";
    out << "cooldown_query_ns=" << cooldown_query_ns << "\n";
    out << "aoe_target_select_ns=" << aoe_target_select_ns << "\n";
    out << "mem_bytes_per_skill_state=" << mem_bytes_per_skill_state << "\n";
    out.close();

    LineFmt("try_cast_ns=%.3f (fail=%llu)\n", try_cast_ns,
            static_cast<unsigned long long>(try_fail));
    LineFmt("cooldown_query_ns=%.3f\n", cooldown_query_ns);
    LineFmt("aoe_target_select_ns=%.3f (mon=%zu, fail=%llu)\n", aoe_target_select_ns, kNumMon,
            static_cast<unsigned long long>(aoe_fail));
    LineFmt("mem_bytes_per_skill_state=%zu (skills=%zu)\n", mem_bytes_per_skill_state,
            h.skill.SkillCount());
    LineFmt("skill_bench done\n");
    return 0;
}
