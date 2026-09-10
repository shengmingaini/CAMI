// scripting/lua/tests/hot_reload_integration_test.cpp —— TASK-032 · 集成测试（§17）
//
// §17 的要求：Scene 以 20Hz 持续跑战斗（脚本在伤害公式中生效），运维线程发起热更 →
//   ① 断言切换发生在 **Tick 边界**（切换前后脚本版本不出现在同一 Tick 内）
//   ② 切换期间 **无 Tick 超时**
//   ③ 切换后新逻辑生效且 **状态连续（脚本全局变量保留）**
//
// 实现方式（刻意用真实组件，不做假）
// -------------------------------
//   * 真实 `mmo::game::SimulationScheduler`（TASK-013）+ 真实 `TickPhase` 固定八阶段；
//   * `CombatStage`（TickPhase::Combat）每 Tick 调一次脚本 —— 脚本就是「伤害公式」；
//   * `ScriptReloadStage`（TickPhase::Replication，**最后一个阶段**）在 Tick 边界开安全点
//     并驱动阶段 4-5 —— 这就是 TASK-032 对 §15 第 4 步「注册 Tick 边界钩子」的落地；
//   * 「运维线程」是一个 `std::thread`，只跑阶段 1-3（Prepare/Validate），
//     **绝不触碰生产 VM** —— 验证 §9 的线程模型真的成立。
//
// 更强的断言技巧：脚本每 Tick 只上报一个值 `counter * multiplier`。由于 counter 每 Tick
//   恰好 +1，C++ 侧可以**反推出** counter 与 multiplier：
//     第 i 次调用（0-based）必须等于 (i+1)*10 或 (i+1)*100。
//   这一条同时锁死三件事：counter 跨切换连续（无重置）、multiplier 只切换一次且不可逆、
//   **不存在任何一次调用落在「半新半旧」状态**。
//
// 输出统一走 test_print.h（红线禁止 cout / printf / cerr）。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/sched/simulation_scheduler.h"
#include "mmo/game/sched/tick_phase.h"
#include "mmo/script/hot_reload/audit_sink.h"
#include "mmo/script/hot_reload/hot_reloader.h"
#include "mmo/script/hot_reload/script_reload_stage.h"
#include "mmo/script/lua_vm.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"
#include "mmo/script/script_value.h"

#include "test_print.h"

namespace {

namespace core = mmo::core;
using core::ErrorCode;
using mmo::game::ISimulationStage;
using mmo::game::SceneContext;
using mmo::game::SceneType;
using mmo::game::SimulationScheduler;
using mmo::game::TickPhase;
using mmo::script::BindingDef;
using mmo::script::BindingKind;
using mmo::script::HotReloader;
using mmo::script::LuaLimits;
using mmo::script::MarkdownAuditSink;
using mmo::script::ScriptCall;
using mmo::script::ScriptContext;
using mmo::script::ScriptReloadStage;
using mmo::script::ScriptValue;

constexpr std::int64_t kIntervalNs = 1'000'000'000LL / 20;  // 20Hz = 50ms

int fails = 0;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ::mmo::core::test::ErrorFmt("FAIL @ %d : %s\n", __LINE__, #cond);     \
            ++fails;                                                              \
        }                                                                         \
    } while (0)

#define CHECK_OK(expr)                                                        \
    do {                                                                      \
        const auto _r = (expr);                                               \
        if (!_r) {                                                            \
            ::mmo::core::test::ErrorFmt("FAIL @ %d : %s -> %s msg=%.*s\n",    \
                                        __LINE__, #expr,                      \
                                        ::mmo::core::ToString(_r.Err().Code()), \
                                        static_cast<int>(_r.Err().Message().size()), \
                                        _r.Err().Message().data());           \
            ++fails;                                                          \
        }                                                                     \
    } while (0)

#define CHECK_FAIL_CODE(expr, code)                                                \
    do {                                                                           \
        const auto _r = (expr);                                                    \
        if (_r) {                                                                  \
            ::mmo::core::test::ErrorFmt("FAIL @ %d : expected failure: %s\n",       \
                                        __LINE__, #expr);                          \
            ++fails;                                                               \
        } else if (_r.Err().Code() != (code)) {                                     \
            ::mmo::core::test::ErrorFmt("FAIL @ %d : code got=%s want=%s\n", __LINE__, \
                                        ::mmo::core::ToString(_r.Err().Code()),         \
                                        ::mmo::core::ToString(code));                  \
            ++fails;                                                               \
        }                                                                          \
    } while (0)

// ---- 共享系统（与 TASK-013 的 sched_sim_test 同模式）----
core::EventBus& Bus() {
    static core::EventBus bus;
    return bus;
}
mmo::game::EntityManager& Mgr() {
    static mmo::game::EntityManager mgr(&Bus());
    return mgr;
}
core::Scheduler& Sch() {
    static core::Scheduler sch;
    return sch;
}
core::Arena& Ara() {
    static core::Arena arena(1 << 16);
    return arena;
}

// ---- 探针：脚本每 Tick 上报一个值 ----
struct Probe {
    static constexpr std::size_t kCapacity = 512;
    std::int64_t values[kCapacity]{};
    std::size_t count{0};
    bool overflow{false};
};

Probe g_probe;

core::Result<int> ProbeTick(ScriptCall& call, void* user) {
    auto* probe = static_cast<Probe*>(user);
    if (probe == nullptr || probe->count >= Probe::kCapacity) {
        if (probe != nullptr) {
            probe->overflow = true;
        }
        return call.Done();
    }
    probe->values[probe->count] = call.ArgCount() > 0 ? call.Arg(0).AsInt() : -1;
    ++probe->count;
    return call.Done();
}

void AddProbe(ScriptContext& ctx) {
    BindingDef def;
    def.name = "probe.tick";
    def.kind = BindingKind::Native;
    def.fn = &ProbeTick;
    def.user = &g_probe;
    def.read_only = true;
    def.description = "per-tick probe";
    const auto added = ctx.AddBinding(std::move(def));
    if (!added) {
        ::mmo::core::test::ErrorFmt("FATAL: cannot add probe binding\n");
        ++fails;
    }
}

// 隔离 VM 的准备回调：装同款绑定，否则冒烟阶段会因「索引 nil」误判脚本不合格。
core::Result<void> PrepareIsolated(ScriptContext& ctx, void* /*user*/) {
    AddProbe(ctx);
    return core::Result<void>::Ok();
}

// ---- Combat 阶段：每 Tick 调一次「伤害公式」脚本 ----
class CombatStage final : public ISimulationStage {
public:
    CombatStage(ScriptContext& ctx, mmo::script::ScriptId id) : ctx_(ctx), id_(id) {}

    TickPhase Phase() const noexcept override { return TickPhase::Combat; }
    std::string_view Name() const noexcept override { return "CombatScriptCall"; }

    void Execute(const SceneContext& ctx) override {
        last_tick_ = ctx.tick_number;
        const auto called = ctx_.Call(id_, "on_tick");
        if (!called) {
            call_failures_ += 1;
        }
        calls_ += 1;
    }

    std::uint64_t Calls() const noexcept { return calls_; }
    std::uint64_t CallFailures() const noexcept { return call_failures_; }
    std::uint64_t LastTick() const noexcept { return last_tick_; }

private:
    ScriptContext& ctx_;
    mmo::script::ScriptId id_;
    std::uint64_t calls_{0};
    std::uint64_t call_failures_{0};
    std::uint64_t last_tick_{0};
};

// ---- 两个版本：只有 multiplier 不同（10 → 100）----
constexpr const char* kSceneName = "scene_damage_formula";

// v1：multiplier 是**脚本常量** 10，counter 是**跨 Tick 累积的状态**。
constexpr const char* kVersion1 = R"LUA(
counter = counter or 0
multiplier = 10
function on_tick()
  counter = counter + 1
  probe.tick(counter * multiplier)
end
)LUA";

// v2：counter 的初始化**必须**保持 `counter or 0`（状态连续），
//     multiplier 则**必须无条件赋值** —— 不能写成 `multiplier or 100`。
//
// 【踩坑 · `or` 默认值会被状态保留语义「正确」地击败】`ReloadInPlace` 会复用旧 `_ENV`
//   并把其中的**非函数值**浅拷贝进新环境（这就是「全局状态保留」的实现）。因此
//   `multiplier = multiplier or 100` 在热更时会看到**旧环境里已被拷过来的 `multiplier=10`**
//   （真值），于是永远保持 10 —— 新逻辑根本不生效，集成测试表现为
//   `v1_calls=12 v2_calls=0`（实测踩到过）。这不是热更 bug，而是测试脚本写法与
//   「状态保留」语义冲突：常量的更新必须显式覆盖。
constexpr const char* kVersion2 = R"LUA(
counter = counter or 0
multiplier = 100
function on_tick()
  counter = counter + 1
  probe.tick(counter * multiplier)
end
)LUA";

}  // namespace

int main() {
    ::mmo::core::test::Line("=== TASK-032 Lua Hot Reload integration test (§17) ===");

    // ---- 生产 VM + 热更器 ----
    auto created = ScriptContext::Create(LuaLimits{});
    if (!created) {
        ::mmo::core::test::ErrorFmt("FATAL: ScriptContext::Create failed\n");
        return 1;
    }
    std::unique_ptr<ScriptContext> ctx = std::move(created).Value();
    AddProbe(*ctx);

    HotReloader reloader(*ctx);
    reloader.SetIsolatedPreparer(&PrepareIsolated, nullptr);

    // 审计落到仓库内的 docs/script-versions.md（§8 阶段 6 的真实交付物）。
    MarkdownAuditSink sink("docs/script-versions.md");
    reloader.SetAuditSink(&sink);

    // ---- 调度器：Combat 阶段调脚本，Replication 阶段是热更安全点 ----
    SimulationScheduler sched(mmo::game::MakeSceneId(0, 7), SceneType::World, 1, Mgr(), Bus(),
                              Sch(), Ara());

    // 先把 v1 上线（首装）。
    auto v1 = reloader.Prepare(kSceneName, kVersion1);
    CHECK_OK(v1);
    if (!v1) {
        return 1;
    }
    CHECK_OK(reloader.Validate(v1.Value()));
    reloader.BeginSafePoint(0);
    CHECK_OK(reloader.Activate(v1.Value(), core::kInvalidTraceId));
    reloader.EndSafePoint();
    CHECK(reloader.CurrentVersion(kSceneName) != nullptr);
    const mmo::script::ScriptId script_id = reloader.CurrentVersion(kSceneName)->id;

    auto combat = std::make_unique<CombatStage>(*ctx, script_id);
    CombatStage* combat_raw = combat.get();
    CHECK_OK(sched.RegisterStage(std::move(combat)));

    auto reload_stage = std::make_unique<ScriptReloadStage>(reloader);
    ScriptReloadStage* reload_raw = reload_stage.get();
    CHECK_OK(sched.RegisterStage(std::move(reload_stage)));

    // ---- 阶段 A：先跑 5 个 Tick（全部走 v1：10,20,30,40,50）----
    core::SteadyTime deadline = core::MonotonicClock::Point();
    auto drive_one = [&]() {
        (void)sched.RunUntil(deadline);
        deadline += std::chrono::nanoseconds(kIntervalNs);
    };

    // ---- 切换点观测（§17 断言 ①的全部证据都在这里）----
    // 两个权威来源：`sched.TickNumber()`（Tick 号的唯一拥有者）与 `ActivatedCount()`（热更计数）。
    //   `activation_tick` = **激活真正发生的那个 Tick**（该 Tick 的 Replication 阶段完成了替换）；
    //   `sample_tick[i]`  = 第 i 次脚本调用（0-based）所在的 Tick —— 由驱动后实测记录，
    //                       不是用「下标 i ⇒ Tick i+1」硬算出来的。
    //
    // 【踩坑 · 逐 Tick 观测，不要在循环外驱动】最初阶段 B 那次 `drive_one()` 写在监视循环之外，
    //   它内部的激活没人观测 → `activation_tick` 被记成「下一轮循环的 Tick」（整体后移一格），
    //   于是 `switch_at == activation_tick` 报错。修法：**所有** Tick 一律经 `drive_and_observe()`
    //   驱动，激活观测就不会漏拍。
    std::uint64_t activation_tick = 0;
    std::uint64_t last_activations = reloader.ActivatedCount();
    std::vector<std::uint64_t> sample_tick;

    auto drive_and_observe = [&]() {
        const std::size_t before = g_probe.count;
        drive_one();
        const std::uint64_t tick = sched.TickNumber();
        for (std::size_t i = before; i < g_probe.count; ++i) {
            sample_tick.push_back(tick);
        }
        if (reloader.ActivatedCount() > last_activations) {
            last_activations = reloader.ActivatedCount();
            activation_tick = tick;
        }
    };

    for (int i = 0; i < 5; ++i) {
        drive_and_observe();
    }
    CHECK(g_probe.count == 5u);
    CHECK(combat_raw->CallFailures() == 0u);
    CHECK(reloader.ActivatedCount() == 1u);
    CHECK(activation_tick == 0u);  // 阶段 A 内没有任何热更
    CHECK(sample_tick.size() == 5u);

    // ---- 阶段 B：运维线程只跑阶段 1-3（不碰生产 VM）----
    // 同时验证：Prepare/Validate 完成后、安全点激活前，线上仍是 v1（§20 验收 #2 的运行时形态）。
    core::Result<void> ops_result = core::Result<void>::Ok();
    bool ops_validated = false;
    std::thread ops([&] {
        const auto ticket = reloader.Prepare(kSceneName, kVersion2);
        if (!ticket) {
            ops_result = core::Result<void>::Fail(ticket.Err());
            return;
        }
        const auto report = reloader.Validate(ticket.Value());
        if (!report) {
            ops_result = core::Result<void>::Fail(report.Err());
            return;
        }
        ops_validated = report.Value().ok;
    });
    ops.join();
    CHECK_OK(ops_result);
    CHECK(ops_validated);
    CHECK(reloader.PendingCount() == 1u);

    // 运维线程已校验通过，但还没到安全点 → 线上必须**仍然**是 v1
    {
        const std::size_t before = g_probe.count;
        drive_and_observe();
        CHECK(g_probe.count == before + 1u);
        // 第 6 次调用 = (6)*10 = 60 → 仍是 v1
        CHECK(g_probe.values[5] == 60);
    }
    // 本 Tick 的 Combat 阶段先于 Replication 阶段执行：Combat 看到的仍是 v1，
    // 随后 Replication 阶段（安全点）才完成原子替换 —— 所以激活归因**正是本 Tick**。
    CHECK(reloader.ActivatedCount() == 2u);
    CHECK(activation_tick == sched.TickNumber());

    // ---- 阶段 C：继续驱动，让 ScriptReloadStage 在 Tick 边界激活 v2 ----
    // 上一轮已经跑掉 1 个 Tick，再跑 6 个覆盖切换点及其后续
    for (int i = 0; i < 6; ++i) {
        drive_and_observe();
    }

    // =====================================================================
    // 断言 1：切换发生在 Tick 边界 —— 反推每次调用的 counter / multiplier
    // =====================================================================
    const std::size_t total = g_probe.count;
    CHECK(total == 12u);          // 5 + 1 + 6
    CHECK(!g_probe.overflow);
    CHECK(combat_raw->CallFailures() == 0u);
    CHECK(combat_raw->Calls() == total);

    std::size_t switch_at = total;  // 首次观察到 v2 的调用下标（0-based）
    for (std::size_t i = 0; i < total; ++i) {
        const std::int64_t counter = static_cast<std::int64_t>(i + 1);
        const std::int64_t expect_v1 = counter * 10;
        const std::int64_t expect_v2 = counter * 100;
        const std::int64_t got = g_probe.values[i];
        if (got == expect_v1) {
            // counter 连续（无重置）+ 尚未切换：一旦切到 v2 就不该再出现 v1（不可逆）
            CHECK(switch_at == total);
        } else if (got == expect_v2) {
            if (switch_at == total) {
                switch_at = i;
            }
        } else {
            // 既不是 v1 也不是 v2 ⇒ counter 断裂 或 出现了半新半旧状态
            CHECK(false);
        }
    }
    CHECK(switch_at > 0u && switch_at < total);
    CHECK(sample_tick.size() == total);
    CHECK(activation_tick > 0u);

    // 关键结论（§17 断言 ①）：切换严格落在 Tick 边界，且**切换前后版本绝不混在同一个 Tick 里**。
    //   ① 第一次观察到 v2 的调用由实测记录定位到它所在的 Tick：`first_v2_tick`；
    //   ② 每 Tick 恰好一次调用（CombatStage）⇒ 调用下标 i 必然落在 Tick i+1：`switch_at + 1 == first_v2_tick`；
    //   ③ 激活发生在 Tick `activation_tick` 的 **Replication**（最后一个阶段），
    //      而该 Tick 的 Combat 已先跑完 ⇒ 同一个 Tick 内不可能出现 v2：`first_v2_tick == activation_tick + 1`。
    //   ①②③ 联合起来即「v2 从激活后的**下一个** Tick 起生效」，版本切换是原子的。
    const std::uint64_t first_v2_tick = sample_tick[switch_at];
    CHECK(switch_at + 1u == first_v2_tick);
    CHECK(first_v2_tick == activation_tick + 1u);

    ::mmo::core::test::LineFmt(
        "  [info] ticks=%llu activation_tick=%llu first_v2_tick=%llu switch_at_call=%zu "
        "v1_calls=%zu v2_calls=%zu\n",
        static_cast<unsigned long long>(sched.TickNumber()),
        static_cast<unsigned long long>(activation_tick),
        static_cast<unsigned long long>(first_v2_tick), switch_at, switch_at, total - switch_at);

    // =====================================================================
    // 断言 2：切换期间无 Tick 超时
    // =====================================================================
    CHECK(sched.OverrunCount() == 0u);
    ::mmo::core::test::LineFmt("  [info] max_tick_us=%llu overrun=%llu\n",
                               static_cast<unsigned long long>(sched.MaxTickUs()),
                               static_cast<unsigned long long>(sched.OverrunCount()));

    // =====================================================================
    // 断言 3：安全点确实每 Tick 开一次；激活/验证计数自洽
    // =====================================================================
    CHECK(reload_raw->SafePoints() == sched.TickNumber());
    CHECK(reload_raw->Activations() == 1u);
    CHECK(combat_raw->LastTick() == sched.TickNumber());
    CHECK(reloader.InSafePoint() == false);  // 阶段结束必须关门
    CHECK(reloader.StateOf(kSceneName) == mmo::script::ReloadState::Activated);
    const auto* ver = reloader.CurrentVersion(kSceneName);
    CHECK(ver != nullptr);
    if (ver != nullptr) {
        CHECK(ver->version == 2u);
    }

    // =====================================================================
    // 断言 4：非安全点发起激活 → 拒绝（集成场景下的 §20 验收 #1）
    // =====================================================================
    const auto late = reloader.Prepare(kSceneName, kVersion1);
    CHECK_OK(late);
    if (late) {
        CHECK_OK(reloader.Validate(late.Value()));
        CHECK_FAIL_CODE(reloader.Activate(late.Value(), core::kInvalidTraceId), ErrorCode::BUSY);
    }

    // =====================================================================
    // 断言 5：审计（阶段 6）—— 只允许在 Tick 之外落盘
    // =====================================================================
    const std::size_t written = reloader.DrainAudit();
    CHECK(written >= 2u);
    CHECK(sink.Records() >= 2u);
    CHECK(reloader.DrainAudit() == 0u);  // 队列已空（幂等）
    ::mmo::core::test::LineFmt("  [info] audit_records=%zu written=%zu path=%s\n",
                               sink.Records(), sink.Written(), sink.Path().c_str());

    if (fails == 0) {
        ::mmo::core::test::Line("ALL HOT RELOAD INTEGRATION TESTS PASSED");
        return 0;
    }
    ::mmo::core::test::ErrorFmt("HOT RELOAD INTEGRATION TESTS FAILED: %d\n", fails);
    return 1;
}
