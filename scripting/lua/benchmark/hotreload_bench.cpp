// scripting/lua/benchmark/hotreload_bench.cpp —— TASK-032 · Benchmark（§18 / §22）
//
// 输出（机器可读 key=value，写入 bench/hotreload.txt，同时回显到 stdout）
//   prepare_ms            单脚本 Prepare（隔离 VM 装配 + 编译）平均毫秒   —— §22 目标 < 10ms
//   validate_ms           单脚本 Validate（静态扫描 + 隔离 VM 冒烟）平均毫秒 —— §22 目标 < 20ms
//   activate_us           单次 Activate（安全点内原子替换）平均微秒 —— **验收阈值 ≤ 100us**
//   activate_us_p99       单次 Activate P99 微秒
//   rollback_us           单次 Rollback（安全点内）平均微秒 —— §22 目标 < 100us
//   activate_batch_ms     一次安全点内激活**全部**脚本的安全点停顿（说明「批量激活会顶破预算」，
//                         故生产应分批/错峰激活；验收断言只看单次 activate_us）
//   scripts / reloads / activations / rollbacks   本次运行参数（便于复现）
//
// 口径说明（禁止把估算值写进报告，§24）
//   - 时间源统一 MonotonicClock（§13 禁止墙钟驱动测量）；SteadyNs 即纳秒整数，无换算损失。
//   - Prepare/Validate 的均值分母是「脚本数 × 轮数」= 真实发生的调用次数，不是外推。
//   - Activate 的样本是**每次单独计时**（100 脚本 × 10 轮 = 1000 个真实样本），
//     P99 由样本排序取分位，不是按均值瞎推。
//   - Activate 必须先 `BeginSafePoint`：非安全点调用会被 §21 的强制点拒绝（返回 BUSY），
//     那样测到的只是错误分支，不是真实停顿。
//   - 热更源码每轮不同（把轮号写进注释）—— checksum 因此每轮必变，
//     Rollback 才有「上一个**不同**版本」可回滚（同 checksum 会被 Rollback 判为无意义而跳过）。

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
#include "mmo/script/hot_reload/hot_reloader.h"
#include "mmo/script/lua_vm.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"
#include "mmo/script/script_value.h"

#include "test_print.h"

using mmo::core::MonotonicClock;
using mmo::core::SteadyNs;
using mmo::script::BindingDef;
using mmo::script::BindingKind;
using mmo::script::HotReloader;
using mmo::script::LuaLimits;
using mmo::script::ReloadTicket;
using mmo::script::ScriptCall;
using mmo::script::ScriptContext;
using mmo::script::ScriptValue;

namespace {

// 本文件在全局命名空间，`core::` 需要显式指向 mmo::core。
namespace core = mmo::core;

/// 空绑定：让脚本里的 `bench.noop(...)` 有真东西可调，避免「索引 nil」在冒烟阶段误判。
core::Result<int> NoopBinding(ScriptCall& call, void* user) {
    (void)user;
    call.SetResult(ScriptValue::Int(1));
    return call.Done();
}

/// 隔离 VM 的准备回调：**必须**与生产 VM 装同样的绑定面，否则凡是用到宿主编好的脚本
/// 都会在隔离 VM 冒烟时因 `attempt to index a nil value` 被误判为不合格（见 hot_reloader.h）。
core::Result<void> InstallBenchBindings(ScriptContext& ctx, void* /*user*/) {
    BindingDef noop;
    noop.name = "bench.noop";
    noop.kind = BindingKind::Native;
    noop.fn = &NoopBinding;
    noop.read_only = true;
    noop.description = "hot reload benchmark noop";
    return ctx.AddBinding(std::move(noop));
}

/// 第 `round` 轮的热更源码。**轮号进注释** ⇒ checksum 每轮必变（Rollback 需要不同版本）。
std::string MakeSource(std::size_t round) {
    std::string src;
    src.reserve(320);
    src += "-- hot reload bench revision ";
    src += std::to_string(round);
    src += "\ncounter = counter or 0\n"
           "function __hot_smoke()\n"
           "  return true\n"
           "end\n"
           "function tick(n)\n"
           "  counter = counter + n\n"
           "  bench.noop(counter)\n"
           "  return counter\n"
           "end\n";
    return src;
}

std::string MakeName(std::size_t i) {
    std::string name = "bench_script_";
    name += std::to_string(i);
    name += ".lua";
    return name;
}

constexpr double kNanosPerMicro = 1'000.0;
constexpr double kNanosPerMilli = 1'000'000.0;

struct Results {
    double prepare_ms{0.0};
    double validate_ms{0.0};
    double activate_us{0.0};
    double activate_us_p99{0.0};
    double rollback_us{0.0};
    double activate_batch_ms{0.0};
    std::size_t scripts{0};
    std::size_t reloads{0};
    std::size_t activations{0};
    std::size_t rollbacks{0};
    bool ok{false};
};

/// 一次「Prepare → Validate」，返回票证（失败时 `ok=false` 并已打印原因）。
bool PrepareAndValidate(HotReloader& hr, const std::string& name, const std::string& source,
                        ReloadTicket* out_ticket, double* prepare_ns, double* validate_ns) {
    const SteadyNs p0 = MonotonicClock::Now();
    const auto prepared = hr.Prepare(name, source);
    const SteadyNs p1 = MonotonicClock::Now();
    if (!prepared) {
        ::mmo::core::test::ErrorFmt("FAIL: Prepare(%s): %.*s\n", name.c_str(),
                                    static_cast<int>(prepared.Err().Message().size()),
                                    prepared.Err().Message().data());
        return false;
    }
    const SteadyNs v0 = MonotonicClock::Now();
    const auto report = hr.Validate(prepared.Value());
    const SteadyNs v1 = MonotonicClock::Now();
    if (!report) {
        ::mmo::core::test::ErrorFmt("FAIL: Validate(%s) returned error\n", name.c_str());
        return false;
    }
    if (!report.Value().ok) {
        ::mmo::core::test::ErrorFmt("FAIL: Validate(%s) not ok, issues=%zu\n", name.c_str(),
                                    report.Value().issues.size());
        return false;
    }
    if (prepare_ns != nullptr) {
        *prepare_ns += static_cast<double>(p1 - p0);
    }
    if (validate_ns != nullptr) {
        *validate_ns += static_cast<double>(v1 - v0);
    }
    if (out_ticket != nullptr) {
        *out_ticket = prepared.Value();
    }
    return true;
}

Results RunAll(std::size_t scripts, std::size_t reloads) {
    Results out;
    out.scripts = scripts;
    out.reloads = reloads;
    if (scripts == 0 || reloads == 0) {
        return out;
    }

    LuaLimits limits;
    limits.max_instructions = 200'000'000u;  // 给足预算，避免 bench 被限额打断
    limits.max_exec_time = core::DurationMs{60'000};

    auto ctx_holder = ScriptContext::Create(limits);
    if (!ctx_holder) {
        ::mmo::core::test::Error("FAIL: ScriptContext::Create failed\n");
        return out;
    }
    ScriptContext& ctx = *ctx_holder.Value();

    BindingDef noop;
    noop.name = "bench.noop";
    noop.kind = BindingKind::Native;
    noop.fn = &NoopBinding;
    noop.read_only = true;
    noop.description = "hot reload benchmark noop";
    if (!ctx.AddBinding(std::move(noop))) {
        ::mmo::core::test::Error("FAIL: AddBinding(bench.noop) failed\n");
        return out;
    }

    HotReloader hr(ctx);
    hr.SetIsolatedPreparer(&InstallBenchBindings, nullptr);

    std::uint64_t tick = 1;

    // ---- 1) 初始部署（不计量）：让每个脚本都「已装载」，热更才走 ReloadInPlace 分支 ----
    for (std::size_t i = 0; i < scripts; ++i) {
        const std::string name = MakeName(i);
        ReloadTicket ticket;
        if (!PrepareAndValidate(hr, name, MakeSource(0), &ticket, nullptr, nullptr)) {
            return out;
        }
        hr.BeginSafePoint(tick++);
        const auto activated = hr.Activate(ticket, core::kInvalidTraceId);
        hr.EndSafePoint();
        if (!activated) {
            ::mmo::core::test::ErrorFmt("FAIL: initial Activate(%s): %.*s\n", name.c_str(),
                                        static_cast<int>(activated.Err().Message().size()),
                                        activated.Err().Message().data());
            return out;
        }
    }

    // ---- 2) 计量 Prepare / Validate / Activate ----
    double prepare_ns_total = 0.0;
    double validate_ns_total = 0.0;
    double batch_ns_total = 0.0;
    std::vector<double> activate_samples;
    activate_samples.reserve(scripts * reloads);

    for (std::size_t round = 1; round <= reloads; ++round) {
        std::vector<ReloadTicket> tickets;
        tickets.reserve(scripts);
        for (std::size_t i = 0; i < scripts; ++i) {
            const std::string name = MakeName(i);
            const std::string src = MakeSource(round * 1000 + i);
            ReloadTicket ticket;
            if (!PrepareAndValidate(hr, name, src, &ticket, &prepare_ns_total,
                                    &validate_ns_total)) {
                return out;
            }
            tickets.push_back(std::move(ticket));
        }

        // 全部票证累积后，在**一个安全点**内激活 —— 这也是 §22 关心的「Tick 尖峰」形态。
        hr.BeginSafePoint(tick);
        const SteadyNs batch0 = MonotonicClock::Now();
        for (const ReloadTicket& ticket : tickets) {
            const SteadyNs a0 = MonotonicClock::Now();
            const auto activated = hr.Activate(ticket, core::kInvalidTraceId);
            const SteadyNs a1 = MonotonicClock::Now();
            if (!activated) {
                hr.EndSafePoint();
                ::mmo::core::test::ErrorFmt("FAIL: Activate(%s): %.*s\n", ticket.name.c_str(),
                                            static_cast<int>(activated.Err().Message().size()),
                                            activated.Err().Message().data());
                return out;
            }
            activate_samples.push_back(static_cast<double>(a1 - a0));
        }
        const SteadyNs batch1 = MonotonicClock::Now();
        batch_ns_total += static_cast<double>(batch1 - batch0);
        // 阶段 5：验证**更早 Tick** 激活的脚本（本次刚激活的还轮不到，判据是 activated_tick < 当前）。
        (void)hr.VerifyPass(core::kInvalidTraceId);
        hr.EndSafePoint();
    }
    ++tick;

    // ---- 3) 计量 Rollback（每脚本一次；此时 history 已有多个不同 checksum 的版本）----
    double rollback_ns_total = 0.0;
    for (std::size_t i = 0; i < scripts; ++i) {
        const std::string name = MakeName(i);
        hr.BeginSafePoint(tick);
        const SteadyNs r0 = MonotonicClock::Now();
        const auto rolled = hr.Rollback(name, core::kInvalidTraceId);
        const SteadyNs r1 = MonotonicClock::Now();
        hr.EndSafePoint();
        if (!rolled) {
            ::mmo::core::test::ErrorFmt("FAIL: Rollback(%s): %.*s\n", name.c_str(),
                                        static_cast<int>(rolled.Err().Message().size()),
                                        rolled.Err().Message().data());
            return out;
        }
        rollback_ns_total += static_cast<double>(r1 - r0);
    }
    ++tick;

    // ---- 4) 诚实性校验：热更后的脚本必须真的可调用（否则数字无意义）----
    std::size_t callable = 0;
    for (std::size_t i = 0; i < scripts; ++i) {
        const std::string name = MakeName(i);
        const auto* version = hr.CurrentVersion(name);
        if (version == nullptr) {
            ::mmo::core::test::ErrorFmt("FAIL: CurrentVersion(%s) is null\n", name.c_str());
            return out;
        }
        if (ctx.Call(version->id, "tick", 1)) {
            ++callable;
        }
    }
    if (callable != scripts) {
        ::mmo::core::test::ErrorFmt("FAIL: only %zu/%zu scripts callable after reload\n", callable,
                                    scripts);
        return out;
    }

    const double calls = static_cast<double>(scripts) * static_cast<double>(reloads);
    out.prepare_ms = calls > 0.0 ? prepare_ns_total / calls / kNanosPerMilli : 0.0;
    out.validate_ms = calls > 0.0 ? validate_ns_total / calls / kNanosPerMilli : 0.0;
    out.activate_batch_ms =
        static_cast<double>(reloads) > 0.0 ? batch_ns_total / static_cast<double>(reloads) /
                                                kNanosPerMilli
                                          : 0.0;

    if (!activate_samples.empty()) {
        double sum = 0.0;
        for (const double sample : activate_samples) {
            sum += sample;
        }
        out.activate_us = sum / static_cast<double>(activate_samples.size()) / kNanosPerMicro;
        std::vector<double> sorted = activate_samples;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t idx = (sorted.size() * 99u) / 100u;
        out.activate_us_p99 = sorted[idx < sorted.size() ? idx : sorted.size() - 1] / kNanosPerMicro;
    }
    out.rollback_us = scripts > 0 ? rollback_ns_total / static_cast<double>(scripts) / kNanosPerMicro
                                  : 0.0;
    out.activations = static_cast<std::size_t>(hr.ActivatedCount());
    out.rollbacks = static_cast<std::size_t>(scripts);
    out.ok = true;
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t scripts = 100;
    std::size_t reloads = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--scripts" && i + 1 < argc) {
            const auto value = std::atoll(argv[++i]);
            scripts = value > 0 ? static_cast<std::size_t>(value) : scripts;
        } else if (arg == "--reloads" && i + 1 < argc) {
            const auto value = std::atoll(argv[++i]);
            reloads = value > 0 ? static_cast<std::size_t>(value) : reloads;
        }
    }

    ::mmo::core::test::Line("== TASK-032 hotreload_bench ==\n");

    const Results r = RunAll(scripts, reloads);
    if (!r.ok) {
        ::mmo::core::test::Error("FAIL: benchmark run aborted\n");
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories("bench", ec);
    std::FILE* fp = std::fopen("bench/hotreload.txt", "wb");
    if (fp == nullptr) {
        ::mmo::core::test::Error("FAIL: cannot open bench/hotreload.txt for writing\n");
        return 1;
    }
    std::fprintf(fp,
                 "prepare_ms=%.4f\n"
                 "validate_ms=%.4f\n"
                 "activate_us=%.3f\n"
                 "activate_us_p99=%.3f\n"
                 "rollback_us=%.3f\n"
                 "activate_batch_ms=%.4f\n"
                 "scripts=%zu\n"
                 "reloads=%zu\n"
                 "activations=%zu\n"
                 "rollbacks=%zu\n",
                 r.prepare_ms, r.validate_ms, r.activate_us, r.activate_us_p99, r.rollback_us,
                 r.activate_batch_ms, r.scripts, r.reloads, r.activations, r.rollbacks);
    std::fclose(fp);

    ::mmo::core::test::LineFmt(
        "prepare_ms=%.4f validate_ms=%.4f\n"
        "activate_us=%.3f activate_us_p99=%.3f rollback_us=%.3f activate_batch_ms=%.4f\n"
        "scripts=%zu reloads=%zu activations=%zu rollbacks=%zu\n",
        r.prepare_ms, r.validate_ms, r.activate_us, r.activate_us_p99, r.rollback_us,
        r.activate_batch_ms, r.scripts, r.reloads, r.activations, r.rollbacks);

    ::mmo::core::test::Line("wrote bench/hotreload.txt\n");
    return 0;
}
