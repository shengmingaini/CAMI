/// TASK-024 · Combat Benchmark（§18 / §22）。
///
/// bin/combat_bench --entities 1000 --combat-ratio 0.5
/// 输出 bench/combat.txt：combat_phase_us_at_1k / cast_resolve_ns / threat_update_ns /
/// alloc_per_combat_tick。
/// 另支持 --matrix（TASK-025）：--duration/--warmup/--out，跑 4 规模 × 5 场景 = 20 组矩阵，
/// 每组 <dir>/combat_<n>_<slug>.txt（tick_avg/p50/p95/p99/max + 八阶段 P95），汇总 combat_matrix.json。
///
/// 热路径零分配由全局 operator new/delete 计数验证（alloc_per_combat_tick ≤ 0）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include "test_print.h"

// TASK-025：--matrix 模式委托 mmo::bench::CombatBenchmark 跑 4 规模 × 5 场景矩阵。
#include "combat_benchmark.h"

// TASK-041：--lua-loaded 回归模式复用 tools/qa 的 header-only 编排器（E2EScenario）。
#include "e2e/e2e_runner.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/combat/buff/buff_system.h"
#include "mmo/game/combat/combat_entity.h"
#include "mmo/game/combat/combat_system.h"
#include "mmo/game/combat/damage/damage.h"
#include "mmo/game/combat/damage/damage_formula.h"
#include "mmo/game/combat/damage/damage_system.h"
#include "mmo/game/combat/skill/combat_events.h"
#include "mmo/game/combat/skill/skill_system.h"
#include "mmo/game/combat/threat_table.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"

// ---- 全局分配计数（热路径零分配验证，§21 Forbidden）----
std::size_t g_alloc_count = 0;
void* operator new(std::size_t n) {
    g_alloc_count += n > 0 ? 1 : 0;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace mmo::game;
using namespace mmo::game::combat;
using namespace mmo::game::role;
namespace core = mmo::core;
using core::test::ErrorFmt;

constexpr SceneId kScene = 1;
constexpr NodeId kOwner = 1;
constexpr const char* kFormulaPath = "config/gameplay/combat/formula.json";
constexpr const char* kBuffPath = "config/gameplay/buffs/buffs.json";
constexpr const char* kSkillDir = "config/gameplay/skills";

class FixedArena final : public core::Arena {
public:
    explicit FixedArena(std::size_t bytes) : core::Arena(bytes) {}
};

struct Harness {
    InMemoryPersistenceAdapter sink;
    core::EventBus bus;
    RoleSystem roles{sink, ExpCurve::FromConfig(ExpCurveConfig{60, 100.0, 1.0}).Value()};
    EntityManager mgr;
    FixedArena arena{1u << 20};
    core::Scheduler sched;
    DamageFormula formula;
    DamageSystem dmg;
    buff::BuffRegistry registry;
    combat::BuffRegistry skill_buffs;
    buff::BuffSystem buffs;
    SkillSystem skills;
    CombatSystem combat;

    explicit Harness()
        : dmg(formula, roles, mgr, kScene, &bus),
          buffs(registry, roles, &dmg, &bus),
          skills(roles, mgr, nullptr, kScene, kOwner, &bus),
          combat(skills, dmg, buffs, roles, mgr, kScene, &bus) {
        auto f = DamageFormula::LoadFromFile(kFormulaPath);
        if (f.HasValue()) formula = f.Value();
        else ErrorFmt("bench: cannot load formula\n");
        if (!registry.LoadFromConfig(kBuffPath).HasValue()) {
            ErrorFmt("bench: cannot load buffs\n");
        }
        // 技能加载期校验引用的 Buff：读 buffs.json 进 combat::BuffRegistry
        {
            std::ifstream in(kBuffPath, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            if (!skill_buffs.Load(ss.str()).HasValue()) {
                ErrorFmt("bench: cannot load skill-buffs\n");
            }
        }
        if (!skills.LoadSkillsFromDir(kSkillDir, skill_buffs).HasValue()) {
            ErrorFmt("bench: cannot load skills\n");
        }
        combat.BindEventBus(bus);
    }

    EntityId Spawn(CharacterId cid, std::int64_t max_hp, std::int64_t attack,
                   std::int64_t defense, EntityType type = EntityType::Player) {
        const auto c = roles.LoadOrCreate(static_cast<PlayerId>(cid), cid,
                                           SceneContext(kScene, SceneType::World, kOwner,
                                                        core::MonotonicClock::Point(), 0, mgr,
                                                        bus, sched, arena));
        if (!c.HasValue()) return 0;
        Character* cp = roles.Find(cid);
        if (cp == nullptr) return 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Agility)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Strength)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Intellect)] = 0;
        cp->attrs.base[static_cast<std::size_t>(AttrType::Stamina)] = 0;
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
        cp->hp = max_hp;
        cp->mp = 1000;  // 施法耗蓝校验
        const auto e = mgr.Create(type, kScene, Position{});
        if (!e.HasValue()) return 0;
        const EntityId id = e.Value()->Id();
        (void)roles.AttachToScene(cid, id, kScene);
        dmg.BindAvatar(id, cid);
        buffs.BindAvatar(id, cid);
        skills.BindAvatar(id, cid);
        combat.BindAvatar(id, cid);
        return id;
    }
};

double RunPhase(Harness& h, std::size_t entities, double ratio, std::size_t rounds,
                std::size_t& allocs_out) {
    // 预热：构造战斗局面
    std::vector<EntityId> ids;
    for (std::size_t i = 0; i < entities; ++i) ids.push_back(h.Spawn(1000 + i, 8000, 200, 100));
    const std::size_t in_combat = static_cast<std::size_t>(entities * ratio);
    auto base = core::MonotonicClock::Point();
    (void)h.combat.Update(SceneContext(kScene, SceneType::World, kOwner, base, 0, h.mgr, h.bus, h.sched, h.arena));
    for (std::size_t i = 0; i < in_combat; ++i) {
        EntityId enemy = ids[(i + 1) % entities];
        (void)h.combat.EnterCombat(ids[i], enemy, 1);
    }
    // 热身几轮
    for (std::size_t r = 0; r < 50; ++r) {
        auto now = base + std::chrono::milliseconds(static_cast<std::int64_t>(r) * 50);
        (void)h.combat.Update(SceneContext(kScene, SceneType::World, kOwner, now, r, h.mgr, h.bus, h.sched, h.arena));
    }
    // 计时轮
    const std::size_t alloc_before = g_alloc_count;
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t r = 0; r < rounds; ++r) {
        auto now = base + std::chrono::milliseconds(static_cast<std::int64_t>(1000 + r) * 50);
        (void)h.combat.Update(SceneContext(kScene, SceneType::World, kOwner, now, 1000 + r, h.mgr, h.bus, h.sched, h.arena));
    }
    auto t1 = std::chrono::steady_clock::now();
    allocs_out = g_alloc_count - alloc_before;
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    return us;
}

// 从可执行文件位置向上找仓库根（含 config/gameplay/scripts.json 的目录）。
std::filesystem::path FindRepoRootArgv(const char* argv0) {
    std::error_code ec;
    std::filesystem::path dir =
        std::filesystem::absolute(std::filesystem::path(argv0), ec).parent_path();
    for (int i = 0; i < 6 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "config" / "gameplay" / "scripts.json", ec)) {
            return dir;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return std::filesystem::current_path(ec);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // 基准测试期间禁止系统休眠（§9 降噪）：避免睡眠/恢复污染 Tick 尾延迟样本。
    ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
#endif
    std::size_t entities = 1000;
    double ratio = 0.5;
    std::size_t ticks = 2000;
    bool matrix = false;
    bool lua_loaded = false;   // TASK-041：跨进程集成 + 战斗性能回归模式
    std::size_t duration = 60;
    std::size_t warmup = 5;
    std::string matrix_out = "bench/combat_matrix.json";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--entities") == 0 && i + 1 < argc)
            entities = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (std::strcmp(argv[i], "--combat-ratio") == 0 && i + 1 < argc)
            ratio = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc)
            ticks = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
            warmup = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            matrix_out = argv[++i];
        else if (std::strcmp(argv[i], "--matrix") == 0)
            matrix = true;
        else if (std::strcmp(argv[i], "--lua-loaded") == 0)
            lua_loaded = true;
    }

    // TASK-041：--lua-loaded 模式（跨进程集成 + 战斗性能回归）。
    // 进程内真实加载 TASK-033 全部 Lua 脚本 + 重跑 TASK-025 战斗性能矩阵 + Lua 探针，
    // 落盘 bench/combat_regression_lua.txt（+ --out 指定的 .json）并施加阈值门禁。
    // 退出码只取决于 tick 阈值门禁（tick_p95≤5000、tick_p99≤8000），与 Lua 是否可用无关：
    // 沙箱（lua=OFF）构建下 Lua 可能不可用，其成败记于 .txt，不阻断 tick 门禁判定。
    if (lua_loaded) {
        mmo::qa::E2EConfig cfg;
        cfg.real_infra = false;          // 沙箱：不触真实 gRPC/Redis/MySQL
        cfg.matrix_full = matrix;        // --matrix 时跑完整 4×5；否则仅 1000/100pct 单场景
        const auto repo = FindRepoRootArgv(argc > 0 ? argv[0] : ".");
        cfg.repo_root = repo.string();
        cfg.lua_manifest = (repo / "config" / "gameplay" / "scripts.json").string();
        cfg.matrix_duration = static_cast<std::uint32_t>(duration);
        cfg.matrix_warmup = static_cast<std::uint32_t>(warmup);
        cfg.bench_out_json = matrix_out;
        cfg.lua_iterations = 20000u;

        mmo::qa::E2EScenario scenario;
        auto run = scenario.Run(cfg);
        if (!run) {
            ErrorFmt("bench: lua-loaded E2E failed: %s\n",
                     std::string(run.Err().Message()).c_str());
            return 2;  // 内部错误（与 tick 门禁失败区分）
        }
        const auto rep = scenario.Report();

        // .txt 路径与 --out 同源（.json → .txt），强制对齐 verify 断言文件名。
        std::string txt_path = matrix_out;
        if (txt_path.size() >= 5 && txt_path.substr(txt_path.size() - 5) == ".json")
            txt_path.replace(txt_path.size() - 5, 5, ".txt");
        else
            txt_path += ".txt";

        std::error_code ec;
        std::filesystem::create_directories("bench", ec);
        {
            std::ofstream f(txt_path, std::ios::binary);
            f << rep.ToText();
        }
        {
            std::ofstream f(matrix_out, std::ios::binary);
            f << "{\n";
            f << "  \"tick_avg_us\": " << rep.tick_avg_us << ",\n";
            f << "  \"tick_p50_us\": " << rep.tick_p50_us << ",\n";
            f << "  \"tick_p95_us\": " << rep.tick_p95_us << ",\n";
            f << "  \"tick_p99_us\": " << rep.tick_p99_us << ",\n";
            f << "  \"tick_max_us\": " << rep.tick_max_us << ",\n";
            f << "  \"lua_load_ok\": " << (rep.lua_load_ok ? 1 : 0) << ",\n";
            f << "  \"lua_scripts_loaded\": " << rep.lua_scripts_loaded << ",\n";
            f << "  \"lua_load_ms\": " << rep.lua_load_ms << ",\n";
            f << "  \"lua_skill_formula_ns\": " << rep.lua_skill_formula_ns << ",\n";
            f << "  \"lua_cpu_percent\": " << rep.lua_cpu_percent << "\n";
            f << "}\n";
        }

        const bool gate = (rep.tick_p95_us <= 5000u) && (rep.tick_p99_us <= 8000u);
        ErrorFmt("bench: lua-loaded regression: tick_p95=%llu tick_p99=%llu lua_ok=%d "
                 "(lua_scripts=%zu lua_ms=%.1f skill_formula_ns=%.1f cpu_pct=%.2f)\n",
                 static_cast<unsigned long long>(rep.tick_p95_us),
                 static_cast<unsigned long long>(rep.tick_p99_us),
                 rep.lua_load_ok ? 1 : 0, rep.lua_scripts_loaded, rep.lua_load_ms,
                 rep.lua_skill_formula_ns, rep.lua_cpu_percent);
        return gate ? 0 : 1;
    }

    // TASK-025：--matrix 跑完整「4 规模 × 5 场景 = 20 组」矩阵（§8 禁止抽样）。
    // 委托 mmo::bench::CombatBenchmark：每组写 <dir>/combat_<n>_<slug>.txt（key=value），
    // 汇总写 <dir>/combat_matrix.json。20Hz 真实节拍，warmup 数据丢弃。
    if (matrix) {
        mmo::bench::CombatBenchmark bm;
        bm.SetDurations(static_cast<std::uint32_t>(duration), static_cast<std::uint32_t>(warmup));
        const std::filesystem::path outp(matrix_out);
        const std::string dir =
            outp.has_parent_path() ? outp.parent_path().string() : std::string(".");
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto r = bm.RunMatrix(dir);
        if (!r.HasValue()) {
            ErrorFmt("bench: matrix failed\n");
            return 1;
        }
        ErrorFmt("bench: matrix done groups=%zu out=%s\n", bm.Results().size(), matrix_out.c_str());
        return 0;
    }

    Harness h;

    std::size_t allocs = 0;
    double us = RunPhase(h, entities, ratio, ticks, allocs);
    double combat_phase_us_at_1k = (us / static_cast<double>(ticks)) * 1000.0 / static_cast<double>(entities);
    double alloc_per_combat_tick = static_cast<double>(allocs) / static_cast<double>(ticks);

    // 单次施法结算耗时（cast_resolve_ns）
    EntityId caster = h.Spawn(5000, 8000, 200, 100);
    EntityId target = h.Spawn(5001, 4000, 150, 80, EntityType::Monster);
    (void)h.combat.Update(SceneContext(kScene, SceneType::World, kOwner, core::MonotonicClock::Point(),
                                 0, h.mgr, h.bus, h.sched, h.arena));
    (void)h.combat.EnterCombat(caster, target, 1);
    // 热身
    for (int i = 0; i < 100; ++i) {
        CastRequest req{}; req.caster = caster; req.skill = 1001; req.target = target; req.trace = 1;
        (void)h.combat.CastSkill(req, SceneContext(kScene, SceneType::World, kOwner,
                                                   core::MonotonicClock::Point(), 0, h.mgr, h.bus, h.sched, h.arena));
        while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();
    }
    auto ct0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) {
        CastRequest req{}; req.caster = caster; req.skill = 1001; req.target = target; req.trace = 1;
        (void)h.combat.CastSkill(req, SceneContext(kScene, SceneType::World, kOwner,
                                                   core::MonotonicClock::Point(), 0, h.mgr, h.bus, h.sched, h.arena));
        while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();
    }
    auto ct1 = std::chrono::steady_clock::now();
    double cast_resolve_ns = std::chrono::duration<double, std::nano>(ct1 - ct0).count() / 2000.0;

    // 单次仇恨更新耗时（threat_update_ns）
    ThreatTable tt;
    auto tt0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) tt.Add(7000 + (i % 16), 10);
    auto tt1 = std::chrono::steady_clock::now();
    double threat_update_ns = std::chrono::duration<double, std::nano>(tt1 - tt0).count() / 2000.0;

    std::ofstream f("bench/combat.txt");
    f << "combat_phase_us_at_1k=" << combat_phase_us_at_1k << "\n";
    f << "cast_resolve_ns=" << cast_resolve_ns << "\n";
    f << "threat_update_ns=" << threat_update_ns << "\n";
    f << "alloc_per_combat_tick=" << alloc_per_combat_tick << "\n";
    f.close();

    ErrorFmt("bench: combat_phase_us_at_1k=%.3f cast_resolve_ns=%.1f threat_update_ns=%.1f "
             "alloc_per_combat_tick=%.0f (entities=%zu ratio=%.2f ticks=%zu)\n",
             combat_phase_us_at_1k, cast_resolve_ns, threat_update_ns, alloc_per_combat_tick,
             entities, ratio, ticks);
    return 0;
}
