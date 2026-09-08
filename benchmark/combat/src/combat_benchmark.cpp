#include "combat_benchmark.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/config/config_manager.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/combat/buff/buff_system.h"
#include "mmo/game/combat/combat_system.h"
#include "mmo/game/combat/damage/damage_formula.h"
#include "mmo/game/combat/damage/damage_system.h"
#include "mmo/game/combat/skill/combat_events.h"
#include "mmo/game/combat/skill/skill_system.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/movement/movement_state.h"
#include "mmo/game/movement/movement_system.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"

namespace {

using namespace mmo::game;
using namespace mmo::game::combat;
using namespace mmo::game::role;
namespace core = mmo::core;
namespace aoin = mmo::game::aoi;
namespace mvn = mmo::game::movement;
using core::test::ErrorFmt;

constexpr SceneId kScene = 1;
constexpr NodeId kOwner = 1;
constexpr const char* kFormulaPath = "config/gameplay/combat/formula.json";
constexpr const char* kBuffPath = "config/gameplay/buffs/buffs.json";
constexpr const char* kSkillDir = "config/gameplay/skills";

constexpr int kTickHz = 20;
constexpr std::uint64_t kTickIntervalUs = 50000;  // 20Hz
constexpr std::int64_t kBenchHp = 10000000;       // Role MaxHp 上限 1e7：保证 60s 内不死亡（负载恒定）
constexpr std::size_t kRepBufBytes = 1u << 20;    // Replication 复用缓冲（零分配）

/// §9 绑核降噪：将 benchmark 线程固定在单一核心，消除 OS 线程迁移抖动，
/// 提升 Tick 耗时测量的可复现性（同 seed 差异 < 5%，§16 / §20）。仅 Windows 生效。
#ifdef _WIN32
static void PinToSingleCore() {
    const DWORD cores = static_cast<DWORD>(std::thread::hardware_concurrency());
    const DWORD idx = cores > 1 ? 1u : 0u;  // 避开 OS 常用 core 0
    ::SetThreadAffinityMask(::GetCurrentThread(), 1u << idx);
}
#else
static void PinToSingleCore() {}
#endif

/// 战斗轮换技能：1001 Fireball(SingleTarget) / 1004 ArcaneMissile(Projectile)，均瞬发。
constexpr std::uint32_t kCastSkills[2] = {1001, 1004};

class FixedArena final : public core::Arena {
public:
    explicit FixedArena(std::size_t bytes) : core::Arena(bytes) {}
};

/// 确定性 PRNG（splitmix64）：同 seed 必然复现同一序列（§15.6 可复现 < 5%）。
struct Rng {
    std::uint64_t s{0};
    explicit Rng(std::uint32_t seed) : s(Mix(static_cast<std::uint64_t>(seed))) {}
    static std::uint64_t Mix(std::uint64_t x) noexcept {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
    std::uint64_t Next() noexcept {
        s += 0x9E3779B97F4A7C15ull;
        return Mix(s);
    }
    float Unit() noexcept {  // [0,1)
        return static_cast<float>(static_cast<double>(Next() >> 40) / 16777216.0);
    }
    float Range(float lo, float hi) noexcept { return lo + (hi - lo) * Unit(); }
};

mvn::MovementConfig MakeMoveConfig() noexcept {
    mvn::MovementConfig c{};
    c.max_speed = 6.0f;
    c.tick_dt = 0.05f;
    // 停止外推关闭：benchmark 里没有客户端命令流，否则 2s 后全员速度清零、Movement 场景失真。
    c.stop_timeout_sec = 1.0e9f;
    return c;
}

/// 被测世界：一次场景一套（规模不同，不跨场景复用）。
struct World {
    InMemoryPersistenceAdapter sink;
    core::EventBus bus;
    RoleSystem roles;
    EntityManager mgr;
    FixedArena arena{1u << 22};
    core::Scheduler sched;
    DamageFormula formula;
    DamageSystem dmg;
    buff::BuffRegistry registry;
    combat::BuffRegistry skill_buffs;
    buff::BuffSystem buffs;
    SkillSystem skills;
    CombatSystem combat;
    std::unique_ptr<aoin::IAoi> aoi;
    mvn::MovementSystem move{MakeMoveConfig()};

    std::vector<EntityId> players;
    std::vector<EntityId> monsters;
    std::vector<EntityId> intent_target;
    std::vector<SkillId> intent_skill;

    std::uint64_t damage_events{0};
    std::uint64_t heal_events{0};

    World()
        : roles(sink, ExpCurve::FromConfig(ExpCurveConfig{60, 100.0, 1.0}).Value()),
          dmg(formula, roles, mgr, kScene, &bus),
          buffs(registry, roles, &dmg, &bus),
          skills(roles, mgr, nullptr, kScene, kOwner, &bus),
          combat(skills, dmg, buffs, roles, mgr, kScene, &bus) {
        const auto f = DamageFormula::LoadFromFile(kFormulaPath);
        if (f.HasValue()) formula = f.Value();
        (void)registry.LoadFromConfig(kBuffPath);
        // 技能加载期校验引用的 Buff：读 buffs.json 进 combat::BuffRegistry
        const auto txt = core::ConfigManager::ReadFile(kBuffPath);
        if (txt.HasValue()) (void)skill_buffs.Load(txt.Value());
        (void)skills.LoadSkillsFromDir(kSkillDir, skill_buffs);
        combat.BindEventBus(bus);
        aoi = aoin::CreateDynamicGridAoi(aoin::AoiConfig{});
        move.BindAoi(*aoi);
        (void)bus.Subscribe<DamageEvent>([this](const DamageEvent&) { ++damage_events; });
        (void)bus.Subscribe<HealEvent>([this](const HealEvent&) { ++heal_events; });
    }

    SceneContext Ctx(core::SteadyTime now, std::uint64_t tick) {
        return SceneContext(kScene, SceneType::World, kOwner, now, tick, mgr, bus, sched, arena);
    }

    EntityId Spawn(CharacterId cid, std::int64_t max_hp, std::int64_t attack, std::int64_t defense,
                   EntityType type, const Position& p) {
        const auto c = roles.LoadOrCreate(static_cast<PlayerId>(cid), cid, Ctx(core::MonotonicClock::Point(), 0));
        if (!c.HasValue()) return 0;
        Character* cp = roles.Find(cid);
        if (cp == nullptr) return 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Agility)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Strength)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Intellect)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Stamina)] = 0;
        auto force = [&](AttrType t, std::int64_t target) {
            cp->attrs.Recompute();
            const std::int64_t cur = cp->attrs.Total(t);
            if (cur != target) {
                cp->attrs.base[static_cast<std::size_t>(t)] += (target - cur);
                cp->attrs.Recompute();
            }
        };
        force(AttrType::Attack, attack);
        force(AttrType::Defense, defense);
        force(AttrType::MaxHp, max_hp);
        cp->hp = max_hp;
        cp->mp = 1000000;  // 60s 连续施法不断蓝，避免 InsufficientResource 干扰负载
        const auto e = mgr.Create(type, kScene, p);
        if (!e.HasValue()) return 0;
        const EntityId id = e.Value()->Id();
        (void)roles.AttachToScene(cid, id, kScene);
        dmg.BindAvatar(id, cid);
        buffs.BindAvatar(id, cid);
        skills.BindAvatar(id, cid);
        combat.BindAvatar(id, cid);
        mvn::MovementState st{};
        st.pos = p;
        st.max_speed = 6.0f;
        (void)move.Register(id, st);
        (void)aoi->Enter(id, p);
        return id;
    }
};

core::Result<void> FailWith(core::ErrorCode code, const char* msg) {
    return core::Result<void>::Fail(core::Error(code, msg));
}

}  // namespace

namespace mmo::bench {

// ===========================================================================
// CombatBenchmark::Impl
// ===========================================================================

struct CombatBenchmark::Impl {
    std::uint32_t seed{42};

    core::Result<void> RunScenario(const ScenarioConfig& cfg, ScenarioResult& out);
};

core::Result<void> CombatBenchmark::Impl::RunScenario(const ScenarioConfig& cfg,
                                                      ScenarioResult& out) {
    if (cfg.player_count == 0) {
        return FailWith(core::ErrorCode::INVALID_ARGUMENT, "player_count must be > 0");
    }

    PinToSingleCore();  // §9 绑核降噪，提升可复现性

    World w;
    Rng rng(cfg.seed);

    // 世界边长 sqrt(n)*12（TASK-014 口径，使 avg_visible 落在 §22 的 20~60 区间）
    const double side = std::sqrt(static_cast<double>(cfg.player_count)) * 12.0;
    const float half = static_cast<float>(side * 0.5);

    const std::size_t n_players = static_cast<std::size_t>(cfg.player_count);
    const std::size_t n_monsters = (n_players / 10) > 0 ? (n_players / 10) : 1;

    for (std::size_t i = 0; i < n_players; ++i) {
        const Position p{rng.Range(-half, half), 0.0f, rng.Range(-half, half), 0.0f};
        const EntityId id = w.Spawn(static_cast<CharacterId>(1000 + i), kBenchHp, 200, 100,
                                    EntityType::Player, p);
        if (id == 0) return FailWith(core::ErrorCode::INTERNAL_ERROR, "spawn player failed");
        w.players.push_back(id);
    }
    for (std::size_t i = 0; i < n_monsters; ++i) {
        const Position p{rng.Range(-half, half), 0.0f, rng.Range(-half, half), 0.0f};
        const EntityId id = w.Spawn(static_cast<CharacterId>(500000 + i), kBenchHp, 150, 80,
                                    EntityType::Monster, p);
        if (id == 0) return FailWith(core::ErrorCode::INTERNAL_ERROR, "spawn monster failed");
        w.monsters.push_back(id);
    }

    const std::size_t n_entities = n_players + n_monsters;
    const std::size_t n_combat = static_cast<std::size_t>(
        static_cast<double>(n_players) * static_cast<double>(cfg.combat_ratio) + 0.5);

    w.intent_target.assign(n_players, 0);
    w.intent_skill.assign(n_players, kCastSkills[0]);
    for (std::size_t i = 0; i < n_players; ++i) {
        w.intent_target[i] = w.monsters[i % n_monsters];
        w.intent_skill[i] = static_cast<SkillId>(kCastSkills[i & 1u]);
    }

    // 进入战斗（双向 InCombat）
    for (std::size_t i = 0; i < n_combat; ++i) {
        (void)w.combat.EnterCombat(w.players[i], w.intent_target[i], 1);
    }

    // ---- Tick 循环（20Hz 真实节拍：工作独立计时，空档 sleep 不计入样本）----
    const std::uint64_t warmup_ticks = static_cast<std::uint64_t>(cfg.warmup_seconds) * kTickHz;
    const std::uint64_t measure_ticks = static_cast<std::uint64_t>(cfg.duration_seconds) * kTickHz;
    const std::uint64_t total_ticks = warmup_ticks + measure_ticks;

    mmo::bench::TickHistogram tick_hist;
    mmo::bench::TickHistogram phase_hist[kPhaseCount];

    std::vector<EntityId> vis;
    std::vector<EntityId> vis_flat;
    std::vector<std::size_t> vis_offsets;
    vis.reserve(256);
    vis_flat.reserve(n_entities * 64);
    vis_offsets.reserve(n_entities + 1);
    std::vector<std::uint8_t> rep_buf(kRepBufBytes, 0);

    std::uint64_t visible_total = 0;
    std::uint64_t msgs_total = 0;
    const std::uint64_t dmg_before = w.damage_events;
    const std::uint64_t heal_before = w.heal_events;

    const core::SteadyTime t_start = core::MonotonicClock::Point();
    const SystemSample sys0 = SampleSystem();
    const auto wall_start = std::chrono::steady_clock::now();
    auto deadline = wall_start;

    std::uint64_t phase_us[kPhaseCount]{};

    for (std::uint64_t t = 0; t < total_ticks; ++t) {
        const bool record = (t >= warmup_ticks);
        const core::SteadyTime now =
            t_start + std::chrono::milliseconds(static_cast<std::int64_t>(t) * 50);
        const SceneContext ctx = w.Ctx(now, t);

        auto run_phase = [&](int idx, auto&& fn) {
            const auto a = std::chrono::steady_clock::now();
            fn();
            const auto b = std::chrono::steady_clock::now();
            const std::uint64_t us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
            phase_us[idx] = us;
            if (record) phase_hist[idx].Record(us);
        };

        // ---- 0 Input：生成本 Tick 意图（目标 / 技能），确定性 ----
        run_phase(0, [&] {
            for (std::size_t i = 0; i < n_players; ++i) {
                const std::uint64_t r = rng.Next();
                w.intent_skill[i] = static_cast<SkillId>(kCastSkills[r & 1u]);
            }
        });

        // ---- 1 Movement ----
        run_phase(1, [&] {
            if (!cfg.movement_enabled) return;
            if ((t % 20u) == 0u) {  // 每 20 Tick（1s）换向一次
                for (std::size_t i = 0; i < w.players.size(); ++i) {
                    const float ang = rng.Range(0.0f, 6.2831853f);
                    const float sp = rng.Range(2.0f, 6.0f);
                    (void)w.move.SetVelocity(w.players[i],
                                             mvn::Vec3{std::cos(ang) * sp, 0.0f, std::sin(ang) * sp});
                }
            }
            (void)w.move.Integrate(ctx, 0.05f);
        });

        // ---- 2 AOI：QueryVisible（可见集供 Replication 消费）----
        run_phase(2, [&] {
            vis_flat.clear();
            vis_offsets.clear();
            for (std::size_t i = 0; i < w.players.size(); ++i) {
                vis_offsets.push_back(vis_flat.size());
                vis.clear();
                (void)w.aoi->QueryVisible(w.players[i], vis);
                vis_flat.insert(vis_flat.end(), vis.begin(), vis.end());
            }
            vis_offsets.push_back(vis_flat.size());
            if (record) visible_total += vis_flat.size();
        });

        // ---- 3 Combat：推进飞行物/读条 + 状态机 + 施法 ----
        run_phase(3, [&] {
            (void)w.skills.Update(ctx);
            (void)w.combat.Update(ctx);
            for (std::size_t i = 0; i < n_combat; ++i) {
                CastRequest req{};
                req.caster = w.players[i];
                req.skill = w.intent_skill[i];
                req.target = w.intent_target[i];
                req.trace = 1;
                (void)w.combat.CastSkill(req, ctx);
            }
        });

        // ---- 4 Buff ----
        run_phase(4, [&] { (void)w.buffs.Tick(ctx); });

        // ---- 5 Quest：本 harness 未接入 Quest 系统，计为 0（报告标注）----
        run_phase(5, [&] { /* 未接入，保持 0µs，不做伪造填充 */ });

        // ---- 6 Event：循环 Drain 直到队列清空（EventBus 单次上限 4096）----
        run_phase(6, [&] {
            while (w.bus.QueueDepth() > 0) (void)w.bus.Drain();
        });

        // ---- 7 Replication：按可见集写下行记录（复用缓冲，零分配）----
        run_phase(7, [&] {
            std::size_t written = 0;
            const std::size_t mask = kRepBufBytes - 1;
            for (std::size_t i = 0; i < n_players && (i + 1) < vis_offsets.size(); ++i) {
                const std::size_t b = vis_offsets[i];
                const std::size_t e = vis_offsets[i + 1];
                for (std::size_t k = b; k < e; ++k) {
                    const std::size_t off = (written * 12u) & mask;
                    const EntityId id = vis_flat[k];
                    std::memcpy(rep_buf.data() + off, &id, sizeof(EntityId));
                    ++written;
                }
            }
            if (record) msgs_total += written;
        });

        std::uint64_t total_us = 0;
        for (int i = 0; i < kPhaseCount; ++i) total_us += phase_us[i];
        if (record) tick_hist.Record(total_us);

        // 节拍：sleep 到下一个 50ms 边界（落后则不 sleep，保证测量的是真实工作压力）
        deadline += std::chrono::microseconds(kTickIntervalUs);
        const auto nowc = std::chrono::steady_clock::now();
        if (nowc < deadline) std::this_thread::sleep_until(deadline);
    }

    const SystemSample sys1 = SampleSystem();
    const auto wall_end = std::chrono::steady_clock::now();
    const double wall_s = std::chrono::duration<double>(wall_end - wall_start).count();
    const std::uint64_t wall_us =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       wall_end - wall_start)
                                       .count());

    out.player_count = cfg.player_count;
    out.combat_ratio = cfg.combat_ratio;
    out.tick_avg_us = static_cast<std::uint64_t>(tick_hist.Average());
    out.tick_p50_us = tick_hist.P50();
    out.tick_p95_us = tick_hist.P95();
    out.tick_p99_us = tick_hist.P99();
    out.tick_max_us = tick_hist.Max();
    for (int i = 0; i < kPhaseCount; ++i) out.phase_us[i] = phase_hist[i].P95();

    const std::uint64_t denom = measure_ticks * static_cast<std::uint64_t>(n_players);
    out.aoi_avg_visible = denom > 0 ? (visible_total / denom) : 0;

    const std::uint64_t events = (w.damage_events - dmg_before) + (w.heal_events - heal_before);
    out.combat_events_per_sec = wall_s > 0.0
                                    ? static_cast<std::uint64_t>(static_cast<double>(events) / wall_s)
                                    : 0;
    out.msgs_out_per_sec =
        wall_s > 0.0 ? static_cast<std::uint64_t>(static_cast<double>(msgs_total) / wall_s) : 0;
    out.total_combat_events = events;  // 确定性签名：同 seed 必然一致（§16 / §20 可复现）
    out.peak_rss_mb = BytesToMb(std::max(sys0.rss_bytes, sys1.rss_bytes));
    out.cpu_percent = CpuPercentBetween(sys0, sys1, wall_us);
    out.tick_count = measure_ticks;
    out.name = std::string(ScenarioName(cfg.kind)) + "-" + std::to_string(cfg.player_count);

    return core::Result<void>::Ok();
}

// ===========================================================================
// CombatBenchmark 公开方法
// ===========================================================================

CombatBenchmark::CombatBenchmark() : impl_(std::make_unique<Impl>()) {}
CombatBenchmark::~CombatBenchmark() = default;

core::Result<ScenarioResult> CombatBenchmark::Run(const ScenarioConfig& cfg) {
    ScenarioResult out{};
    auto r = impl_->RunScenario(cfg, out);
    if (!r.HasValue()) return core::Result<ScenarioResult>::Fail(r.Err());
    results_.push_back(out);
    return core::Result<ScenarioResult>::Ok(out);
}

core::Result<void> CombatBenchmark::ExportJson(const ScenarioResult& r,
                                               std::string_view path) const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"name\": \"" << r.name << "\",\n";
    ss << "  \"player_count\": " << r.player_count << ",\n";
    ss << "  \"combat_ratio\": " << r.combat_ratio << ",\n";
    ss << "  \"tick_avg_us\": " << r.tick_avg_us << ",\n";
    ss << "  \"tick_p50_us\": " << r.tick_p50_us << ",\n";
    ss << "  \"tick_p95_us\": " << r.tick_p95_us << ",\n";
    ss << "  \"tick_p99_us\": " << r.tick_p99_us << ",\n";
    ss << "  \"tick_max_us\": " << r.tick_max_us << ",\n";
    ss << "  \"tick_count\": " << r.tick_count << ",\n";
    ss << "  \"aoi_avg_visible\": " << r.aoi_avg_visible << ",\n";
    ss << "  \"combat_events_per_sec\": " << r.combat_events_per_sec << ",\n";
    ss << "  \"msgs_out_per_sec\": " << r.msgs_out_per_sec << ",\n";
    ss << "  \"peak_rss_mb\": " << r.peak_rss_mb << ",\n";
    ss << "  \"cpu_percent\": " << r.cpu_percent << ",\n";
    ss << "  \"phase_p95_us\": {";
    for (int i = 0; i < kPhaseCount; ++i) {
        if (i != 0) ss << ", ";
        ss << "\"" << PhaseName(i) << "\": " << r.phase_us[i];
    }
    ss << "}\n";
    ss << "}\n";

    std::error_code ec;
    const std::filesystem::path p{std::string(path)};
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return FailWith(core::ErrorCode::INTERNAL_ERROR, "cannot open export json path");
    f << ss.str();
    f.close();
    return core::Result<void>::Ok();
}

core::Result<void> CombatBenchmark::ExportMarkdown(std::string_view path) const {
    std::ostringstream ss;
    ss << "| 规模 | 场景 | Avg(µs) | P50(µs) | P95(µs) | P99(µs) | Max(µs) | AOI可见 | 战斗事件/s | RSS(MB) | CPU(%) |\n";
    ss << "|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const ScenarioResult& r : results_) {
        ss << "| " << r.player_count << " | " << r.name << " | " << r.tick_avg_us << " | "
           << r.tick_p50_us << " | " << r.tick_p95_us << " | " << r.tick_p99_us << " | "
           << r.tick_max_us << " | " << r.aoi_avg_visible << " | " << r.combat_events_per_sec
           << " | " << r.peak_rss_mb << " | " << r.cpu_percent << " |\n";
    }
    ss << "\n### 八阶段 P95 分解（µs）\n\n| 场景 |";
    for (int i = 0; i < kPhaseCount; ++i) ss << " " << PhaseName(i) << " |";
    ss << "\n|---|";
    for (int i = 0; i < kPhaseCount; ++i) ss << "---|";
    ss << "\n";
    for (const ScenarioResult& r : results_) {
        ss << "| " << r.name << " |";
        for (int i = 0; i < kPhaseCount; ++i) ss << " " << r.phase_us[i] << " |";
        ss << "\n";
    }

    std::error_code ec;
    const std::filesystem::path p{std::string(path)};
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return FailWith(core::ErrorCode::INTERNAL_ERROR, "cannot open markdown path");
    f << ss.str();
    f.close();
    return core::Result<void>::Ok();
}

core::Result<void> CombatBenchmark::RunMatrix(std::string_view output_dir) {
    const std::string dir(output_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    results_.clear();
    const std::vector<ScenarioConfig> cfgs = MatrixConfigs(duration_seconds_, warmup_seconds_);
    if (cfgs.size() != 20) {
        return FailWith(core::ErrorCode::INTERNAL_ERROR, "matrix must contain exactly 20 groups");
    }

    for (const ScenarioConfig& cfg : cfgs) {
        ScenarioResult r{};
        auto res = impl_->RunScenario(cfg, r);
        if (!res.HasValue()) return core::Result<void>::Fail(res.Err());
        results_.push_back(r);

        // 每组落盘 key=value（供 assert_metric 断言，禁止后处理）
        const std::string file =
            dir + "/combat_" + std::to_string(cfg.player_count) + "_" + ScenarioSlug(cfg.kind) + ".txt";
        std::ofstream f(file, std::ios::binary);
        if (!f) return FailWith(core::ErrorCode::INTERNAL_ERROR, "cannot write scenario txt");
        f << "tick_avg_us=" << r.tick_avg_us << "\n";
        f << "tick_p50_us=" << r.tick_p50_us << "\n";
        f << "tick_p95_us=" << r.tick_p95_us << "\n";
        f << "tick_p99_us=" << r.tick_p99_us << "\n";
        f << "tick_max_us=" << r.tick_max_us << "\n";
        f << "tick_count=" << r.tick_count << "\n";
        f << "aoi_avg_visible=" << r.aoi_avg_visible << "\n";
        f << "combat_events_per_sec=" << r.combat_events_per_sec << "\n";
        f << "msgs_out_per_sec=" << r.msgs_out_per_sec << "\n";
        f << "peak_rss_mb=" << r.peak_rss_mb << "\n";
        f << "cpu_percent=" << r.cpu_percent << "\n";
        for (int i = 0; i < kPhaseCount; ++i) {
            f << "phase_p95_us_" << PhaseName(i) << "=" << r.phase_us[i] << "\n";
        }
        f.close();

        ErrorFmt("bench: %-18s avg=%llu p95=%llu p99=%llu max=%llu visible=%llu cpu=%.1f%% rss=%lluMB\n",
                 r.name.c_str(), static_cast<unsigned long long>(r.tick_avg_us),
                 static_cast<unsigned long long>(r.tick_p95_us),
                 static_cast<unsigned long long>(r.tick_p99_us),
                 static_cast<unsigned long long>(r.tick_max_us),
                 static_cast<unsigned long long>(r.aoi_avg_visible), r.cpu_percent,
                 static_cast<unsigned long long>(r.peak_rss_mb));
    }

    // 汇总 JSON
    std::ostringstream js;
    js << "{\n  \"scenarios\": [\n";
    for (std::size_t i = 0; i < results_.size(); ++i) {
        const ScenarioResult& r = results_[i];
        if (i != 0) js << ",\n";
        js << "    {\"name\": \"" << r.name << "\", \"player_count\": " << r.player_count
           << ", \"combat_ratio\": " << r.combat_ratio << ", \"tick_avg_us\": " << r.tick_avg_us
           << ", \"tick_p95_us\": " << r.tick_p95_us << ", \"tick_p99_us\": " << r.tick_p99_us
           << ", \"tick_max_us\": " << r.tick_max_us << ", \"aoi_avg_visible\": " << r.aoi_avg_visible
           << ", \"peak_rss_mb\": " << r.peak_rss_mb << "}";
    }
    js << "\n  ]\n}\n";
    std::ofstream jf(dir + "/combat_matrix.json", std::ios::binary);
    if (!jf) return FailWith(core::ErrorCode::INTERNAL_ERROR, "cannot write matrix json");
    jf << js.str();
    jf.close();

    return core::Result<void>::Ok();
}

}  // namespace mmo::bench
