#pragma once

/// TASK-041 · 跨进程集成与战斗性能回归 —— 纯验证方公共接口（§7 冻结契约）。
///
/// 本模块不拥有任何服务状态（§4 State Owner），只消费 TASK-025（战斗性能矩阵）、
/// TASK-030（账本对账）、TASK-033（Lua 脚本全集）的公开接口做回归，禁止改动被验证实现。
///
/// 沙箱可跑子集（本交付物范围）：
///   - 进程内真实加载 TASK-033 全部 Lua 脚本（compile→validate→activate）；
///   - 进程内真实重跑 TASK-025 战斗性能矩阵（含 1000 玩家 / 100% Combat 场景）；
///   - 真实测量 TASK-033 单次「技能公式」hook 开销与脚本层 CPU 占比；
///   - 阈值门禁判定（tick_p95≤5000、tick_p99≤8000、退化<5%、Lua 零装载错误）。
/// 需真实基础设施的部分（gRPC Bot 走 Login→…→Persist、TASK-030 economy_audit 五场景故障对账）
/// 在 `E2EConfig::real_infra=true` 路径下显式返回「需 Gateway+GameNode+DataService+Redis+MySQL」，
/// 沙箱不执行（见 REGRESSION.md）。
///
/// 本文件为 header-only：被 server/gamenode/combat 的 combat_bench 驱动与 tools/qa 的
/// ctest 同时包含，故所有定义均为 inline，避免多 TU 的 ODR 问题。

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/bus/command_bus.h"
#include "mmo/core/time/clock.h"

#include "combat_benchmark.h"
#include "scenario.h"

#include "mmo/gameplay/gameplay_script_host.h"
#include "mmo/gameplay/gameplay_types.h"

#include "mmo/script/script_event.h"

namespace mmo::qa {

/// 回归运行配置。
struct E2EConfig {
    bool real_infra = false;  ///< true = 真实 gRPC+Redis+MySQL 跨进程 E2E（沙箱无基础设施，显式报错）
    bool matrix_full = true;  ///< true = 跑完整 4×5 矩阵（--matrix）；false = 仅 1000/100pct 单场景（ctest 加速）
    std::string repo_root;   ///< 仓库根（用于拼接 TASK-033 清单里的相对脚本路径）
    std::string bench_out_json = "bench/combat_regression_lua.json";  ///< --out 路径
    std::string lua_manifest;                                          ///< scripts.json 路径（CWD=仓库根 时相对亦可）
    std::uint32_t matrix_duration = 60;
    std::uint32_t matrix_warmup = 5;
    std::uint32_t lua_iterations = 20000;  ///< Lua hook 探针迭代数
};

/// 阈值判定结论。
struct RegressionVerdict {
    bool pass = false;
    std::uint64_t tick_p95_us = 0;
    std::uint64_t tick_p99_us = 0;
    double degradation_pct = 0.0;  ///< 相对基线退化百分比（>5% 判失败；基线为 0 时跳过该判定）
    std::string reason;
};

/// 一份完整回归报告（机器可读，落盘 combat_regression_lua.txt / .json）。
struct RegressionReport {
    std::uint64_t tick_avg_us = 0;
    std::uint64_t tick_p50_us = 0;
    std::uint64_t tick_p95_us = 0;
    std::uint64_t tick_p99_us = 0;
    std::uint64_t tick_max_us = 0;

    bool lua_load_ok = false;
    std::size_t lua_scripts_loaded = 0;
    std::vector<std::string> lua_load_errors;
    double lua_load_ms = 0.0;

    double lua_skill_formula_ns = 0.0;  ///< TASK-033 单次「技能公式」hook 平均开销（ns）
    double lua_cpu_percent = 0.0;       ///< TASK-033 脚本层 CPU 占世界时间的百分比

    std::uint64_t baseline_tick_p95_us = 0;  ///< 历史基线（沙箱未持久化时为 0，跳过退化判定）
    std::uint64_t baseline_tick_p99_us = 0;

    RegressionVerdict verdict;

    std::string ToText() const;                          ///< 生成 bench/combat_regression_lua.txt 内容
    static RegressionReport FromText(std::string_view text);  ///< 解析（供单测与回归报告比对）
};

/// 阈值判定（§8 / §22）：tick_p95≤5000、tick_p99≤8000、Lua 零装载错误、退化<5%。
inline RegressionVerdict EvaluateVerdict(const RegressionReport& report);

/// 跨进程 E2E 编排器（纯验证方）。
class E2EScenario {
public:
    E2EScenario() = default;

    /// 进程内回归（real_infra=false 时真实跑 Lua 加载 + 战斗矩阵 + Lua 探针）；
    /// real_infra=true 显式失败（需 Gateway+GameNode+DataService+Redis+MySQL）。
    core::Result<void> Run(const E2EConfig& cfg);

    /// 取最近一次 Run 的报告。
    RegressionReport Report() const { return report_; }

private:
    RegressionReport report_;
};

// ===========================================================================
// 实现细节（header-only；全部 inline，供 combat_bench 驱动与 qa_e2e_test 共用）
// ===========================================================================

namespace {

using core::MonotonicClock;
using core::SteadyNs;
namespace gp = mmo::gameplay;
namespace bench = mmo::bench;

// 复刻 TASK-033 gameplay_script_bench 的空落地端：脚本受控写不落盘，仅测开销。
inline core::Result<mmo::script::ScriptReply> NullHandler(const mmo::script::ScriptCommand&, void*) {
    mmo::script::ScriptReply reply;
    reply.valid = true;
    return core::Result<mmo::script::ScriptReply>::Ok(reply);
}

// 从可执行文件位置向上找仓库根（含 config/gameplay/scripts.json 的目录）。
inline std::filesystem::path FindRepoRoot(const char* argv0) {
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

struct LuaLoad {
    bool ok = false;
    std::size_t count = 0;
    double ms = 0.0;
    std::vector<std::string> errors;
    std::unique_ptr<gp::GameplayScriptHost> host;
};

inline LuaLoad LoadLua(const E2EConfig& cfg) {
    LuaLoad out;
    const auto t0 = std::chrono::steady_clock::now();

    gp::GameplayScriptHost::Config config;
    config.require_all_scripts = true;
    config.root = cfg.repo_root;  // Config 字段是 root（拼相对脚本路径），非 repo_root

    auto created = gp::GameplayScriptHost::Create(config);
    if (!created) {
        out.errors.push_back(std::string(created.Err().Message()));
        return out;
    }
    out.host = std::move(created).Value();

    core::CommandBus commands;
    if (!out.host->BindCommandBus(commands).HasValue()) {
        out.errors.push_back("BindCommandBus failed");
        return out;
    }
    out.host->BindCommandHandler(&NullHandler, nullptr);

    auto loaded = out.host->LoadManifestFromFile(cfg.lua_manifest);
    const auto t1 = std::chrono::steady_clock::now();
    out.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!loaded) {
        out.errors.push_back(std::string(loaded.Err().Message()));
        return out;
    }
    out.ok = true;
    out.count = loaded.Value();  // LoadManifestFromFile 返回成功装载的脚本数
    for (const auto& e : out.host->LoadErrors()) out.errors.push_back(e);
    return out;
}

inline void ProbeLua(gp::GameplayScriptHost& host, std::uint32_t iterations,
                     double& out_skill_formula_ns, double& out_cpu_percent) {
    constexpr std::size_t kWarmup = 2000;
    const std::size_t n = static_cast<std::size_t>(iterations);

    gp::SkillFormulaRequest req;
    req.skill_id = "fireball";
    req.caster = 1;
    req.target = 2;
    req.attack_power = 100.0;
    req.target_hp_pct = 100.0;
    req.caster_level = 60;

    for (std::size_t i = 0; i < kWarmup && i < n; ++i) {
        (void)host.ComputeSkillFormula(req);
    }
    const SteadyNs b = MonotonicClock::Now();
    for (std::size_t i = 0; i < n; ++i) {
        (void)host.ComputeSkillFormula(req);
    }
    const SteadyNs e = MonotonicClock::Now();
    const double total_ns = static_cast<double>(e - b);
    out_skill_formula_ns = (n > 0u) ? (total_ns / static_cast<double>(n)) : 0.0;

    // 脚本层 CPU 占比：模拟 1s 世界时间，每帧对 50 实体各做技能公式 + NPC 决策，
    // 外加拿一次 Boss 阶段检查、一次活动查询、一次宿主 Tick（§22 口径，缩小规模以加速单测）。
    gp::AiContext ai;
    ai.profile = "aggressive_guard";
    ai.self = 1;
    ai.target = 2;
    ai.hp_pct = 90.0;
    ai.distance = 1.0;

    gp::BossContext boss;
    boss.boss_template = "dragon";
    boss.boss = 9001;
    boss.hp_pct = 69.0;
    boss.phase = 1;

    gp::ActivityQuery act;
    act.activity_id = "double_exp";
    act.player = 1;
    act.now_ms = 2000000;
    act.exp_gain = 500;

    const std::uint32_t hz = 10;
    const std::size_t frames = static_cast<std::size_t>(hz) * 1u;  // 1 秒

    const SteadyNs b2 = MonotonicClock::Now();
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t e3 = 0; e3 < 50u; ++e3) {
            (void)host.ComputeSkillFormula(req);
            (void)host.DecideAi(ai);
        }
        (void)host.CheckBossPhase(boss);
        (void)host.QueryActivity(act);
        (void)host.Tick(1.0 / static_cast<double>(hz));
    }
    const SteadyNs e2t = MonotonicClock::Now();
    const double cpu_ns = static_cast<double>(e2t - b2);
    const double world_ns = 1.0 * 1000000000.0;
    out_cpu_percent = cpu_ns / world_ns * 100.0;
}

inline std::string MatrixDir(std::string_view out_json) {
    std::filesystem::path p{std::string(out_json)};
    if (p.has_parent_path()) return p.parent_path().string();
    return "bench";
}

inline std::uint64_t ParseU64(std::string_view v) {
    try {
        return static_cast<std::uint64_t>(std::stoull(std::string(v)));
    } catch (...) {
        return 0u;
    }
}

}  // namespace

inline RegressionVerdict EvaluateVerdict(const RegressionReport& r) {
    RegressionVerdict v;
    v.tick_p95_us = r.tick_p95_us;
    v.tick_p99_us = r.tick_p99_us;

    // 门禁（§8 / §22）：tick_p95 ≤ 5000、tick_p99 ≤ 8000
    const bool gate = (r.tick_p95_us <= 5000u) && (r.tick_p99_us <= 8000u);
    const bool lua_ok = r.lua_load_ok;

    bool degrad_ok = true;
    if (r.baseline_tick_p95_us > 0u) {
        const double deg =
            (static_cast<double>(r.tick_p95_us) - static_cast<double>(r.baseline_tick_p95_us)) /
            static_cast<double>(r.baseline_tick_p95_us) * 100.0;
        v.degradation_pct = deg;
        if (deg > 5.0) degrad_ok = false;
    }

    v.pass = gate && lua_ok && degrad_ok;
    if (!gate) {
        v.reason = "tick threshold exceeded (p95<=5000,p99<=8000)";
    } else if (!lua_ok) {
        v.reason = "lua scripts failed to load";
    } else if (!degrad_ok) {
        v.reason = "regression > 5% vs baseline";
    } else {
        v.reason = "ok";
    }
    return v;
}

inline core::Result<void> E2EScenario::Run(const E2EConfig& cfg) {
    if (cfg.real_infra) {
        // 真实跨进程 E2E 需要 Gateway+GameNode+DataService+Redis+MySQL 在线，沙箱无基础设施。
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR,
            "real-infra E2E requires running Gateway+GameNode+DataService+Redis+MySQL; "
            "run in a full environment (this sandbox has no external services)"));
    }

    RegressionReport rep;

    // 1) TASK-033 Lua 加载（真实；失败仅记录，不阻塞矩阵）
    LuaLoad lua = LoadLua(cfg);
    rep.lua_load_ok = lua.ok;
    rep.lua_scripts_loaded = lua.count;
    rep.lua_load_errors = std::move(lua.errors);
    rep.lua_load_ms = lua.ms;

    // 2) TASK-025 战斗性能矩阵（真实，进程内）
    bench::CombatBenchmark bench;
    bench.SetDurations(cfg.matrix_duration, cfg.matrix_warmup);

    if (cfg.matrix_full) {
        auto rm = bench.RunMatrix(MatrixDir(cfg.bench_out_json));
        if (!rm) return core::Result<void>::Fail(rm.Err());
        for (const auto& r : bench.Results()) {
            if (r.player_count == 1000u && r.combat_ratio >= 0.999f) {
                rep.tick_avg_us = r.tick_avg_us;
                rep.tick_p50_us = r.tick_p50_us;
                rep.tick_p95_us = r.tick_p95_us;
                rep.tick_p99_us = r.tick_p99_us;
                rep.tick_max_us = r.tick_max_us;
            }
        }
    } else {
        bench::ScenarioConfig sc = bench::MakeScenario(bench::ScenarioKind::Combat100, 1000u,
                                                      cfg.matrix_duration, cfg.matrix_warmup, 42u);
        auto res = bench.Run(sc);
        if (!res) return core::Result<void>::Fail(res.Err());
        const auto& r = res.Value();
        rep.tick_avg_us = r.tick_avg_us;
        rep.tick_p50_us = r.tick_p50_us;
        rep.tick_p95_us = r.tick_p95_us;
        rep.tick_p99_us = r.tick_p99_us;
        rep.tick_max_us = r.tick_max_us;
    }

    // 3) TASK-033 Lua hook 开销探针（真实）
    if (lua.ok && lua.host) {
        ProbeLua(*lua.host, cfg.lua_iterations, rep.lua_skill_formula_ns, rep.lua_cpu_percent);
    }

    rep.verdict = EvaluateVerdict(rep);
    report_ = std::move(rep);
    return core::Result<void>::Ok();
}

inline std::string RegressionReport::ToText() const {
    std::ostringstream ss;
    ss << "tick_avg_us=" << tick_avg_us << "\n";
    ss << "tick_p50_us=" << tick_p50_us << "\n";
    ss << "tick_p95_us=" << tick_p95_us << "\n";
    ss << "tick_p99_us=" << tick_p99_us << "\n";
    ss << "tick_max_us=" << tick_max_us << "\n";
    ss << "lua_scripts_loaded=" << lua_scripts_loaded << "\n";
    ss << "lua_load_ok=" << (lua_load_ok ? 1 : 0) << "\n";
    ss << "lua_load_ms=" << lua_load_ms << "\n";
    ss << "lua_skill_formula_ns=" << lua_skill_formula_ns << "\n";
    ss << "lua_cpu_percent=" << lua_cpu_percent << "\n";
    ss << "baseline_tick_p95_us=" << baseline_tick_p95_us << "\n";
    ss << "baseline_tick_p99_us=" << baseline_tick_p99_us << "\n";
    ss << "verdict_pass=" << (verdict.pass ? 1 : 0) << "\n";
    ss << "verdict_reason=" << verdict.reason << "\n";
    for (const auto& e : lua_load_errors) ss << "lua_error=" << e << "\n";
    return ss.str();
}

inline RegressionReport RegressionReport::FromText(std::string_view text) {
    RegressionReport r;
    std::istringstream iss;
    iss.str(std::string(text));
    std::string line;
    while (std::getline(iss, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "tick_avg_us")
            r.tick_avg_us = ParseU64(val);
        else if (key == "tick_p50_us")
            r.tick_p50_us = ParseU64(val);
        else if (key == "tick_p95_us")
            r.tick_p95_us = ParseU64(val);
        else if (key == "tick_p99_us")
            r.tick_p99_us = ParseU64(val);
        else if (key == "tick_max_us")
            r.tick_max_us = ParseU64(val);
        else if (key == "lua_scripts_loaded")
            r.lua_scripts_loaded = static_cast<std::size_t>(ParseU64(val));
        else if (key == "lua_load_ok")
            r.lua_load_ok = (val == "1");
        else if (key == "lua_load_ms")
            r.lua_load_ms = std::stod(val);
        else if (key == "lua_skill_formula_ns")
            r.lua_skill_formula_ns = std::stod(val);
        else if (key == "lua_cpu_percent")
            r.lua_cpu_percent = std::stod(val);
        else if (key == "verdict_pass")
            r.verdict.pass = (val == "1");
        else if (key == "verdict_reason")
            r.verdict.reason = val;
        else if (key == "lua_error")
            r.lua_load_errors.push_back(val);
    }
    return r;
}

}  // namespace mmo::qa
