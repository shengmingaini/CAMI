// scripting/lua/benchmark/lua_bench.cpp —— TASK-031 · Benchmark（§18 / §22）
//
// 输出（机器可读 key=value，写入 bench/lua.txt，同时回显到 stdout）
//   lua_call_ns             单次脚本调用（0 参数）平均开销 —— **验收阈值 ≤ 2000ns**
//   lua_call_ns_p99         单次脚本调用 P99
//   lua_binding_call_ns     单次「脚本 → C++ 绑定」调用平均开销（技能/Buff 公式的典型形态）
//   lua_load_ms_per_script  单脚本加载（编译 + 执行顶层）平均毫秒
//   lua_load_100_ms         加载 100 个脚本的总耗时（§22 目标 < 100ms）
//   mem_overhead_bytes      单个空 VM 常驻内存（§22 目标 < 4MB）
//   mem_after_100_scripts   加载 100 个脚本后的 VM 内存
//   instruction_check_ns    debug hook 单次检查开销（指令计数 + 栈深 + 挂钟）
//   instr_overhead_pct      指令计数相对「几乎不检查」的额外开销百分比（§22 目标 < 10%）
//   calls / hook_period / scripts   本次运行的参数（便于复现）
//
// 口径说明（禁止把估算值写进报告，§24）
//   - 时间源统一 MonotonicClock（§13 禁止墙钟驱动测量）。
//   - 每次测量前 `warmup` 若干次，避免首次调用把 page-fault / 缓存冷启动算进平均值。
//   - `instruction_check_ns` 用「总时间 / hook 被调用次数（实测计数差值）」得出，
//     分子分母都来自真实执行，不是按指令条数估算。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/script/lua_vm.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"

#include "test_print.h"

using mmo::core::MonotonicClock;
using mmo::core::SteadyNs;
using mmo::script::BindingDef;
using mmo::script::BindingKind;
using mmo::script::LuaLimits;
using mmo::script::ScriptCall;
using mmo::script::ScriptContext;
using mmo::script::ScriptValue;

namespace {

// 本文件在全局命名空间，`core::` 需要显式指向 mmo::core。
namespace core = mmo::core;

/// 空绑定：只测「进入 C++ 绑定 → 返回」的固定开销（不含业务逻辑）。
core::Result<int> NoopBinding(ScriptCall& call, void* user) {
    (void)user;
    call.SetResult(ScriptValue::Int(1));
    return call.Done();
}

double NsToMs(double ns) { return ns / 1'000'000.0; }

struct Results {
    double call_ns{0.0};
    double call_ns_p99{0.0};
    double binding_call_ns{0.0};
    double load_ms_per_script{0.0};
    double load_100_ms{0.0};
    double mem_overhead_bytes{0.0};
    double mem_after_100_scripts{0.0};
    double instruction_check_ns{0.0};
    double instr_overhead_pct{0.0};
    std::size_t calls{0};
    std::size_t hook_period{0};
    std::size_t scripts{0};
};

/// 采样 `calls` 次脚本调用，返回平均与 P99（纳秒）。
void MeasureCalls(ScriptContext& ctx, mmo::script::ScriptId id, std::size_t calls,
                  std::vector<double>& samples_out, double* avg_out, double* p99_out) {
    constexpr std::size_t kWarmup = 1000;
    for (std::size_t i = 0; i < kWarmup && i < calls; ++i) {
        (void)ctx.Call(id, "entry");
    }

    samples_out.clear();
    samples_out.reserve(calls);
    const SteadyNs start = MonotonicClock::Now();
    for (std::size_t i = 0; i < calls; ++i) {
        const SteadyNs t0 = MonotonicClock::Now();
        (void)ctx.Call(id, "entry");
        const SteadyNs t1 = MonotonicClock::Now();
        samples_out.push_back(static_cast<double>(t1 - t0));
    }
    const SteadyNs end = MonotonicClock::Now();

    *avg_out = calls > 0 ? static_cast<double>(end - start) / static_cast<double>(calls) : 0.0;

    std::vector<double> sorted = samples_out;
    std::sort(sorted.begin(), sorted.end());
    if (p99_out == nullptr) {
        return;
    }
    if (sorted.empty()) {
        *p99_out = 0.0;
        return;
    }
    const std::size_t idx = (sorted.size() * 99u) / 100u;
    *p99_out = sorted[idx < sorted.size() ? idx : sorted.size() - 1];
}

Results RunAll(std::size_t calls, std::uint32_t hook_period, std::size_t script_count) {
    Results out;
    out.calls = calls;
    out.hook_period = hook_period;
    out.scripts = script_count;

    LuaLimits limits;
    limits.max_instructions = 200'000'000u;  // 给足预算，避免 bench 被限额打断
    limits.max_exec_time = mmo::core::DurationMs{60'000};

    // ---------------- 1) 空 VM 常驻内存 ----------------
    {
        auto vm = mmo::script::LuaVM::Create(limits);
        if (!vm) {
            ::mmo::core::test::Error("FAIL: LuaVM::Create failed\n");
            return out;
        }
        out.mem_overhead_bytes =
            static_cast<double>(vm.Value()->MemoryUsed());  // 含沙箱全局表 + 3 个白名单库表
    }

    // ---------------- 2) 脚本调用开销 ----------------
    auto ctx_holder = ScriptContext::Create(limits);
    if (!ctx_holder) {
        ::mmo::core::test::Error("FAIL: ScriptContext::Create failed\n");
        return out;
    }
    ScriptContext& ctx = *ctx_holder.Value();
    ctx.Vm().SetHookPeriod(hook_period);

    BindingDef noop;
    noop.name = "bench.noop";
    noop.kind = BindingKind::Native;
    noop.fn = &NoopBinding;
    noop.read_only = true;
    noop.description = "benchmark noop";
    if (!ctx.AddBinding(std::move(noop))) {
        ::mmo::core::test::Error("FAIL: AddBinding failed\n");
        return out;
    }

    // entry：0 参数空函数（测调用框架的固定开销）
    // bind_entry：脚本 → C++ 绑定一次（测绑定通道开销）
    // spin：K 条 Lua 指令（测指令计数开销）
    const auto entry = ctx.Load("bench_entry.lua", "function entry() end");
    const auto bind_entry = ctx.Load("bench_bind.lua", "function entry() bench.noop() end");
    if (!entry || !bind_entry) {
        ::mmo::core::test::Error("FAIL: bench scripts failed to load\n");
        return out;
    }

    std::vector<double> samples;
    MeasureCalls(ctx, entry.Value(), calls, samples, &out.call_ns, &out.call_ns_p99);
    MeasureCalls(ctx, bind_entry.Value(), calls, samples, &out.binding_call_ns, nullptr);

    // ---------------- 3) 脚本加载开销 ----------------
    {
        auto loader = ScriptContext::Create(limits);
        if (!loader) {
            return out;
        }
        // 100 个脚本各带一个函数与一张表，避免「空脚本加载」失真
        const auto t0 = MonotonicClock::Now();
        std::size_t ok = 0;
        for (std::size_t i = 0; i < script_count; ++i) {
            std::string src = "local M = {}\n"
                              "function M.act(n)\n"
                              "  local acc = 0\n"
                              "  for k = 1, n do acc = acc + k end\n"
                              "  return acc\n"
                              "end\n"
                              "return M\n";
            char name[64];
            std::snprintf(name, sizeof(name), "bench_script_%zu.lua", i);
            if (loader.Value()->Load(name, src)) {
                ++ok;
            }
        }
        const auto t1 = MonotonicClock::Now();
        const double total_ms = NsToMs(static_cast<double>(t1 - t0));
        out.load_ms_per_script = ok > 0 ? total_ms / static_cast<double>(ok) : 0.0;
        // §22 目标是「加载 100 个脚本 < 100ms」。本次实测的是单脚本真实均值，
        // 这里按同一口径外推到 100 个（外推值仅作对照，报告同时给出单脚本实测值）。
        out.load_100_ms = out.load_ms_per_script * 100.0;
        out.mem_after_100_scripts = static_cast<double>(loader.Value()->MemoryUsed());
    }

    // ---------------- 4) 指令计数开销 ----------------
    {
        // spin 脚本：一段固定长度的纯 Lua 计算循环（不调绑定，排除绑定开销干扰）
        auto spinner = ScriptContext::Create(limits);
        if (!spinner) {
            return out;
        }
        spinner.Value()->Vm().SetHookPeriod(hook_period);
        const auto id = spinner.Value()->Load(
            "bench_spin.lua",
            "function entry()\n"
            "  local acc = 0\n"
            "  for i = 1, 1000000 do acc = acc + i end\n"
            "  return acc\n"
            "end");
        if (!id) {
            return out;
        }
        constexpr std::size_t kRuns = 20;
        const std::size_t checks_before = spinner.Value()->Vm().InstructionChecks();
        const auto t0 = MonotonicClock::Now();
        for (std::size_t i = 0; i < kRuns; ++i) {
            (void)spinner.Value()->Call(id.Value(), "entry");
        }
        const auto t1 = MonotonicClock::Now();
        const std::size_t checks = spinner.Value()->Vm().InstructionChecks() - checks_before;
        out.instruction_check_ns =
            checks > 0 ? static_cast<double>(t1 - t0) / static_cast<double>(checks) : 0.0;

        // 对照：把检查周期拉到极大（≈不检查）再跑同样次数，得到计数开销的相对占比
        spinner.Value()->Vm().SetHookPeriod(1'000'000'000u);
        const auto t2 = MonotonicClock::Now();
        for (std::size_t i = 0; i < kRuns; ++i) {
            (void)spinner.Value()->Call(id.Value(), "entry");
        }
        const auto t3 = MonotonicClock::Now();
        const double with_hook = static_cast<double>(t1 - t0);
        const double without_hook = static_cast<double>(t3 - t2);
        out.instr_overhead_pct =
            without_hook > 0.0 ? (with_hook - without_hook) / without_hook * 100.0 : 0.0;
    }

    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t calls = 1000000;
    std::size_t scripts = 100;
    std::uint32_t hook_period = 1000;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--calls" && i + 1 < argc) {
            const auto value = std::atoll(argv[++i]);
            calls = value > 0 ? static_cast<std::size_t>(value) : calls;
        } else if (arg == "--scripts" && i + 1 < argc) {
            const auto value = std::atoll(argv[++i]);
            scripts = value > 0 ? static_cast<std::size_t>(value) : scripts;
        } else if (arg == "--hook-period" && i + 1 < argc) {
            const auto value = std::atoll(argv[++i]);
            hook_period = value > 0 ? static_cast<std::uint32_t>(value) : hook_period;
        }
    }

    ::mmo::core::test::Line("== TASK-031 lua_bench ==\n");

    const Results r = RunAll(calls, hook_period, scripts);

    std::error_code ec;
    std::filesystem::create_directories("bench", ec);
    std::FILE* fp = std::fopen("bench/lua.txt", "wb");
    if (fp == nullptr) {
        ::mmo::core::test::Error("FAIL: cannot open bench/lua.txt for writing\n");
        return 1;
    }
    std::fprintf(fp,
                 "lua_call_ns=%.1f\n"
                 "lua_call_ns_p99=%.1f\n"
                 "lua_binding_call_ns=%.1f\n"
                 "lua_load_ms_per_script=%.4f\n"
                 "lua_load_100_ms=%.3f\n"
                 "mem_overhead_bytes=%.0f\n"
                 "mem_after_100_scripts=%.0f\n"
                 "instruction_check_ns=%.1f\n"
                 "instr_overhead_pct=%.3f\n"
                 "calls=%zu\n"
                 "hook_period=%zu\n"
                 "scripts=%zu\n",
                 r.call_ns, r.call_ns_p99, r.binding_call_ns, r.load_ms_per_script, r.load_100_ms,
                 r.mem_overhead_bytes, r.mem_after_100_scripts, r.instruction_check_ns,
                 r.instr_overhead_pct, r.calls, static_cast<std::size_t>(r.hook_period),
                 r.scripts);
    std::fclose(fp);

    ::mmo::core::test::LineFmt(
        "lua_call_ns=%.1f lua_call_ns_p99=%.1f lua_binding_call_ns=%.1f\n"
        "lua_load_ms_per_script=%.4f lua_load_100_ms=%.3f\n"
        "mem_overhead_bytes=%.0f mem_after_100_scripts=%.0f\n"
        "instruction_check_ns=%.1f instr_overhead_pct=%.3f\n"
        "calls=%zu hook_period=%zu scripts=%zu\n",
        r.call_ns, r.call_ns_p99, r.binding_call_ns, r.load_ms_per_script, r.load_100_ms,
        r.mem_overhead_bytes, r.mem_after_100_scripts, r.instruction_check_ns,
        r.instr_overhead_pct, r.calls, static_cast<std::size_t>(r.hook_period), r.scripts);

    ::mmo::core::test::Line("wrote bench/lua.txt\n");
    return 0;
}
