// scripting/gameplay/benchmark/gameplay_script_bench.cpp — TASK-033 · Benchmark（§18 / §22）
//
// 输出（机器可读 key=value，写入 bench/gameplay.txt，同时回显到 stdout）
//   skill_formula_ns              单次「技能公式」hook 端到端平均开销 —— **验收阈值 ≤ 3000ns**
//   skill_formula_ns_p99          单次技能公式 hook 的 P99
//   quest_script_ns               单次「任务事件」hook（DispatchQuestEvent，1 个脚本认领）
//   ai_decide_ns                  单次「NPC 决策」hook（DecideAi）
//   boss_phase_check_ns           单次「Boss 阶段」hook（CheckBossPhase）
//   activity_query_ns             单次「活动修正」hook（QueryActivity）
//   script_total_cpu_percent      脚本层 CPU 占「世界时间」的百分比 —— **验收阈值 ≤ 10**
//   script_total_cpu_ms_per_sec   同上的绝对值（毫秒 / 世界秒），便于人工判读
//   scripts_loaded                实际装载的脚本数（禁止用「以为装了 15 个」代替实测）
//   iterations / sim_entities / sim_tick_hz / sim_seconds   本次运行的参数（可复现）
//
// 口径说明（§24：禁止把估算值写进报告）
// ------------------------------------
//   - 时间源统一 MonotonicClock（§13 禁止墙钟驱动测量）；每次测量前 warmup，避免把
//     首次调用的 page-fault / 缓存冷启动算进平均值。
//   - `script_total_cpu_percent` 的定义是**实测比例**，不是估算：
//       分子 = 模拟 `sim_seconds` 秒世界时间所消耗的全部脚本层 CPU 纳秒（含 Tick 内所有 hook）
//       分母 = `sim_seconds` × 1e9 纳秒
//     即「脚本占满一秒钟世界时间需要多少 CPU」。它回答的是 §22 的问题：
//     把多少计算塞进 Lua 才不会拖垮 Tick 预算。
//   - 负载形状：每帧对 `sim_entities` 个实体各做一次技能公式 + NPC 决策，另加 Boss 阶段
//     与活动修正查询，再调一次宿主 `Tick`（周期脚本 ≤ 1Hz）。
//
// 红线：本文件不出现 std::cout / printf / std::cerr；输出走 test_print.h 与 fopen/fprintf。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "mmo/core/bus/command_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/gameplay/gameplay_script_host.h"
#include "mmo/gameplay/gameplay_types.h"
#include "mmo/script/script_event.h"

#include "test_print.h"

namespace {

namespace core = mmo::core;
namespace fs = std::filesystem;
namespace gp = mmo::gameplay;

using mmo::core::MonotonicClock;
using mmo::core::SteadyNs;

/// 脚本受控写的落地端：bench 只关心「脚本层的开销」，因此是零成本的空实现。
core::Result<mmo::script::ScriptReply> NullHandler(const mmo::script::ScriptCommand& cmd,
                                                  void* user) {
    (void)cmd;
    (void)user;
    mmo::script::ScriptReply reply;
    reply.valid = true;
    return core::Result<mmo::script::ScriptReply>::Ok(reply);
}

double Ns(double value) { return value; }

/// 从可执行文件位置向上找仓库根（含 config/gameplay/scripts.json 的目录）。
/// 直接依赖 cwd 会让「从 build/Release/bin 手动跑」失败，而验收脚本又从仓库根跑。
fs::path FindRepoRoot(const char* argv0) {
    std::error_code ec;
    fs::path dir = fs::absolute(fs::path(argv0), ec).parent_path();
    for (int i = 0; i < 6 && !dir.empty(); ++i) {
        if (fs::exists(dir / "config" / "gameplay" / "scripts.json", ec)) {
            return dir;
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    // 回退：cwd（验收脚本的调用口径）。
    return fs::current_path(ec);
}

struct Results {
    double skill_formula_ns{0.0};
    double skill_formula_ns_p99{0.0};
    double quest_script_ns{0.0};
    double ai_decide_ns{0.0};
    double boss_phase_check_ns{0.0};
    double activity_query_ns{0.0};
    double script_total_cpu_percent{0.0};
    double script_total_cpu_ms_per_sec{0.0};
    std::size_t scripts_loaded{0};
    std::size_t iterations{0};
    std::size_t sim_entities{0};
    std::uint32_t sim_tick_hz{0};
    std::uint32_t sim_seconds{0};
};

double P99(std::vector<double>& samples) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t idx = (samples.size() * 99u) / 100u;
    return samples[idx < samples.size() ? idx : samples.size() - 1];
}

Results RunAll(gp::GameplayScriptHost& host, std::size_t iterations, std::size_t sim_entities,
               std::uint32_t sim_tick_hz, std::uint32_t sim_seconds) {
    Results out;
    out.iterations = iterations;
    out.sim_entities = sim_entities;
    out.sim_tick_hz = sim_tick_hz;
    out.sim_seconds = sim_seconds;
    out.scripts_loaded = host.Stats().loaded;

    constexpr std::size_t kWarmup = 2000;

    // ---------------- 1) 技能公式（§22 阈值指标） ----------------
    {
        gp::SkillFormulaRequest request;
        request.skill_id = "fireball";
        request.caster = 1;
        request.target = 2;
        request.attack_power = 100.0;
        request.target_hp_pct = 100.0;
        request.caster_level = 60;

        for (std::size_t i = 0; i < kWarmup && i < iterations; ++i) {
            (void)host.ComputeSkillFormula(request);
        }
        std::vector<double> samples;
        samples.reserve(iterations);
        const SteadyNs begin = MonotonicClock::Now();
        for (std::size_t i = 0; i < iterations; ++i) {
            const SteadyNs t0 = MonotonicClock::Now();
            (void)host.ComputeSkillFormula(request);
            const SteadyNs t1 = MonotonicClock::Now();
            samples.push_back(static_cast<double>(t1 - t0));
        }
        const SteadyNs end = MonotonicClock::Now();
        out.skill_formula_ns =
            iterations > 0 ? static_cast<double>(end - begin) / static_cast<double>(iterations)
                           : 0.0;
        out.skill_formula_ns_p99 = P99(samples);
    }

    // ---------------- 2) 任务事件（经 CommandBus 转为业务命令） ----------------
    {
        gp::GameplayPayload payload;
        (void)payload.SetName("monster_killed");
        (void)payload.AddInt("monster_id", 1001);
        (void)payload.AddInt("count", 1);
        (void)payload.AddInt("player", 1);

        for (std::size_t i = 0; i < kWarmup && i < iterations; ++i) {
            (void)host.DispatchQuestEvent(payload);
        }
        const SteadyNs begin = MonotonicClock::Now();
        for (std::size_t i = 0; i < iterations; ++i) {
            (void)host.DispatchQuestEvent(payload);
        }
        const SteadyNs end = MonotonicClock::Now();
        out.quest_script_ns =
            iterations > 0 ? static_cast<double>(end - begin) / static_cast<double>(iterations)
                           : 0.0;
    }

    // ---------------- 3) NPC 决策 ----------------
    {
        gp::AiContext ctx;
        ctx.profile = "aggressive_guard";
        ctx.self = 1;
        ctx.target = 2;
        ctx.hp_pct = 90.0;
        ctx.distance = 1.0;
        ctx.state = 0;

        for (std::size_t i = 0; i < kWarmup && i < iterations; ++i) {
            (void)host.DecideAi(ctx);
        }
        const SteadyNs begin = MonotonicClock::Now();
        for (std::size_t i = 0; i < iterations; ++i) {
            (void)host.DecideAi(ctx);
        }
        const SteadyNs end = MonotonicClock::Now();
        out.ai_decide_ns =
            iterations > 0 ? static_cast<double>(end - begin) / static_cast<double>(iterations)
                           : 0.0;
    }

    // ---------------- 4) Boss 阶段 ----------------
    {
        gp::BossContext ctx;
        ctx.boss_template = "dragon";
        ctx.boss = 9001;
        ctx.hp_pct = 69.0;
        ctx.phase = 1;
        ctx.enraged = 0;

        for (std::size_t i = 0; i < kWarmup && i < iterations; ++i) {
            (void)host.CheckBossPhase(ctx);
        }
        const SteadyNs begin = MonotonicClock::Now();
        for (std::size_t i = 0; i < iterations; ++i) {
            (void)host.CheckBossPhase(ctx);
        }
        const SteadyNs end = MonotonicClock::Now();
        out.boss_phase_check_ns =
            iterations > 0 ? static_cast<double>(end - begin) / static_cast<double>(iterations)
                           : 0.0;
    }

    // ---------------- 5) 活动修正 ----------------
    {
        gp::ActivityQuery query;
        query.activity_id = "double_exp";
        query.player = 1;
        query.now_ms = 2'000'000;  // 落在活动窗口内
        query.exp_gain = 500;

        for (std::size_t i = 0; i < kWarmup && i < iterations; ++i) {
            (void)host.QueryActivity(query);
        }
        const SteadyNs begin = MonotonicClock::Now();
        for (std::size_t i = 0; i < iterations; ++i) {
            (void)host.QueryActivity(query);
        }
        const SteadyNs end = MonotonicClock::Now();
        out.activity_query_ns =
            iterations > 0 ? static_cast<double>(end - begin) / static_cast<double>(iterations)
                           : 0.0;
    }

    // ---------------- 6) 脚本层 CPU 占比（§22 阈值指标） ----------------
    //
    // 模拟 `sim_seconds` 秒世界时间：每帧（1 / sim_tick_hz 秒）对 sim_entities 个实体
    // 各做一次技能公式 + NPC 决策，另加一次 Boss 阶段检查与一次活动查询，最后推进宿主 Tick。
    {
        gp::SkillFormulaRequest skill;
        skill.skill_id = "fireball";
        skill.attack_power = 100.0;
        skill.target_hp_pct = 100.0;
        skill.caster_level = 60;

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

        gp::ActivityQuery activity;
        activity.activity_id = "double_exp";
        activity.player = 1;
        activity.now_ms = 2'000'000;
        activity.exp_gain = 500;

        const double frame_dt = 1.0 / static_cast<double>(sim_tick_hz == 0 ? 1 : sim_tick_hz);
        const std::size_t frames = static_cast<std::size_t>(sim_tick_hz) * sim_seconds;

        // warmup：先跑完整一轮，避免把第一帧的冷启动算进去
        for (std::size_t e = 0; e < sim_entities; ++e) {
            (void)host.ComputeSkillFormula(skill);
            (void)host.DecideAi(ai);
        }
        (void)host.CheckBossPhase(boss);
        (void)host.QueryActivity(activity);
        (void)host.Tick(frame_dt);

        const SteadyNs begin = MonotonicClock::Now();
        for (std::size_t f = 0; f < frames; ++f) {
            for (std::size_t e = 0; e < sim_entities; ++e) {
                (void)host.ComputeSkillFormula(skill);
                (void)host.DecideAi(ai);
            }
            (void)host.CheckBossPhase(boss);
            (void)host.QueryActivity(activity);
            (void)host.Tick(frame_dt);
        }
        const SteadyNs end = MonotonicClock::Now();

        const double cpu_ns = static_cast<double>(end - begin);
        const double world_ns = static_cast<double>(sim_seconds) * 1'000'000'000.0;
        out.script_total_cpu_ms_per_sec =
            sim_seconds > 0 ? (cpu_ns / static_cast<double>(sim_seconds)) / 1'000'000.0 : 0.0;
        out.script_total_cpu_percent = world_ns > 0.0 ? cpu_ns / world_ns * 100.0 : 0.0;
    }

    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 100'000;
    std::size_t sim_entities = 120;
    std::uint32_t sim_tick_hz = 10;
    std::uint32_t sim_seconds = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](std::size_t dflt) -> std::size_t {
            if (i + 1 >= argc) {
                return dflt;
            }
            const long long value = std::atoll(argv[++i]);
            return value > 0 ? static_cast<std::size_t>(value) : dflt;
        };
        if (arg == "--iterations") {
            iterations = next(iterations);
        } else if (arg == "--entities") {
            sim_entities = next(sim_entities);
        } else if (arg == "--tick-hz") {
            sim_tick_hz = static_cast<std::uint32_t>(next(sim_tick_hz));
        } else if (arg == "--seconds") {
            sim_seconds = static_cast<std::uint32_t>(next(sim_seconds));
        }
    }

    const fs::path repo_root = FindRepoRoot(argv[0]);

    gp::GameplayScriptHost::Config config;
    config.root = repo_root.string();  // 清单里的 path 相对仓库根
    config.require_all_scripts = true;
    core::Result<std::unique_ptr<gp::GameplayScriptHost>> created =
        gp::GameplayScriptHost::Create(config);
    if (!created) {
        const std::string why(created.Err().Message());
        ::mmo::core::test::ErrorFmt("FAIL: host create: %s\n", why.c_str());
        return 1;
    }
    auto host = std::move(created).Value();

    // 为了测「脚本层的真实开销」而不是「未绑定命令通道的报错路径」，
    // 这里必须装上与生产同形的落地端（空实现，零成本）。
    core::CommandBus commands;
    if (!host->BindCommandBus(commands)) {
        ::mmo::core::test::Error("FAIL: BindCommandBus\n");
        return 1;
    }
    host->BindCommandHandler(&NullHandler, nullptr);

    const std::string manifest_path = (repo_root / "config" / "gameplay" / "scripts.json").string();
    const core::Result<std::size_t> loaded = host->LoadManifestFromFile(manifest_path);
    if (!loaded) {
        const std::string why(loaded.Err().Message());
        ::mmo::core::test::ErrorFmt("FAIL: LoadManifest(%s): %s\n", manifest_path.c_str(),
                                    why.c_str());
        return 1;
    }

    ::mmo::core::test::Line("== TASK-033 gameplay_script_bench ==\n");

    const Results r = RunAll(*host, iterations, sim_entities, sim_tick_hz, sim_seconds);

    // 输出路径：优先 cwd 下的 bench/gameplay.txt（验收脚本口径）；
    // 若 cwd 不可写则退回仓库根，并把实际路径打出来（禁止「静默写到别处」）。
    std::error_code ec;
    fs::create_directories("bench", ec);
    std::FILE* fp = std::fopen("bench/gameplay.txt", "wb");
    std::string out_path = "bench/gameplay.txt";
    if (fp == nullptr) {
        fs::create_directories(repo_root / "bench", ec);
        out_path = (repo_root / "bench" / "gameplay.txt").string();
        fp = std::fopen(out_path.c_str(), "wb");
    }
    if (fp == nullptr) {
        ::mmo::core::test::Error("FAIL: cannot open bench/gameplay.txt for writing\n");
        return 1;
    }
    std::fprintf(fp,
                 "skill_formula_ns=%.1f\n"
                 "skill_formula_ns_p99=%.1f\n"
                 "quest_script_ns=%.1f\n"
                 "ai_decide_ns=%.1f\n"
                 "boss_phase_check_ns=%.1f\n"
                 "activity_query_ns=%.1f\n"
                 "script_total_cpu_percent=%.4f\n"
                 "script_total_cpu_ms_per_sec=%.4f\n"
                 "scripts_loaded=%zu\n"
                 "iterations=%zu\n"
                 "sim_entities=%zu\n"
                 "sim_tick_hz=%u\n"
                 "sim_seconds=%u\n",
                 Ns(r.skill_formula_ns), Ns(r.skill_formula_ns_p99), Ns(r.quest_script_ns),
                 Ns(r.ai_decide_ns), Ns(r.boss_phase_check_ns), Ns(r.activity_query_ns),
                 r.script_total_cpu_percent, r.script_total_cpu_ms_per_sec, r.scripts_loaded,
                 r.iterations, r.sim_entities, r.sim_tick_hz, r.sim_seconds);
    std::fclose(fp);

    ::mmo::core::test::LineFmt(
        "skill_formula_ns=%.1f skill_formula_ns_p99=%.1f\n"
        "quest_script_ns=%.1f ai_decide_ns=%.1f\n"
        "boss_phase_check_ns=%.1f activity_query_ns=%.1f\n"
        "script_total_cpu_percent=%.4f script_total_cpu_ms_per_sec=%.4f\n"
        "scripts_loaded=%zu iterations=%zu\n"
        "sim_entities=%zu sim_tick_hz=%u sim_seconds=%u\n",
        Ns(r.skill_formula_ns), Ns(r.skill_formula_ns_p99), Ns(r.quest_script_ns),
        Ns(r.ai_decide_ns), Ns(r.boss_phase_check_ns), Ns(r.activity_query_ns),
        r.script_total_cpu_percent, r.script_total_cpu_ms_per_sec, r.scripts_loaded, r.iterations,
        r.sim_entities, r.sim_tick_hz, r.sim_seconds);

    ::mmo::core::test::LineFmt("wrote %s (repo_root=%s)\n", out_path.c_str(),
                               repo_root.string().c_str());
    return 0;
}
