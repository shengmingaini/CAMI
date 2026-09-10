// scripting/lua/tests/hot_reload_test.cpp —— TASK-032 · 单元测试（§16）+ 失败路径测试（§19）
//
// 自包含 harness（与 TASK-031 / engine/core/tests 同口径）：纯断言 + 失败计数器，
// 不依赖 gtest。输出统一走 test_print.h 的 fwrite 通道（红线禁止 cout / printf / cerr）。
//
// 覆盖清单（对应 §16 / §19 / §20 Acceptance Criteria）
//   §16  六阶段状态机全路径 · 编译失败/校验失败时旧版零影响 · 非安全点 Activate 被拒 ·
//        原子替换后新旧函数引用正确 · 回滚恢复旧版本 · 版本历史截断到 5 · checksum 计算
//   §19  磁盘脚本文件被删 → Prepare 返回 NOT_FOUND · 连续 10 次热更历史正确截断且不泄漏 ·
//        运行期崩溃 → 自动回滚（§20 验收 #4）
//   §20  #1 Activate 只能在 Tick Safe Point · #2 失败不影响线上旧版 · #4 自动回滚 ·
//        #5 版本历史保留最近 5 个 · #6 每次激活有审计记录
//   §9   线程模型：Prepare/Validate 可跨线程（Worker），Activate 跨线程 → BUSY
//   §17  脚本全局变量跨热更保留（ReloadInPlace 语义的核心断言）
//
// 脚本写法约定（重要，继承 TASK-031）
// --------------------------------
//   `Call(id, fn)` 在**脚本模块表**里查函数；模块表 = chunk 返回的 table，或（不返回时）
//   该脚本私有的 `_ENV`。因此测试脚本一律用**全局函数定义** `function go() ... end`。
//
// 隔离 VM 的绑定（TASK-032 特有）
// -----------------------------
//   `Prepare` / `Validate` 在隔离临时 VM 上冒烟，该 VM **默认没有任何宿主绑定**。
//   所以必须用 `SetIsolatedPreparer` 把与生产 VM 相同的绑定装进去，否则凡是用到
//   `probe.*` 的脚本都会在冒烟阶段因「索引 nil」被误判为不合格。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/time/clock.h"
#include "mmo/script/hot_reload/audit_sink.h"
#include "mmo/script/hot_reload/hot_reloader.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"
#include "mmo/script/script_value.h"
#include "mmo/script/lua_vm.h"

#include "test_print.h"

using mmo::core::ErrorCode;
using mmo::script::BindingDef;
using mmo::script::BindingKind;
using mmo::script::HotReloader;
using mmo::script::LuaLimits;
using mmo::script::MarkdownAuditSink;
using mmo::script::ReloadState;
using mmo::script::ScriptCall;
using mmo::script::ScriptContext;
using mmo::script::ScriptValue;
using mmo::script::ToString;

namespace {

namespace core = mmo::core;

int failures = 0;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s\n", __FILE__, __LINE__, \
                                        #cond);                                   \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

#define CHECK_FAIL_CODE(expr, code)                                                  \
    do {                                                                             \
        const auto _r = (expr);                                                      \
        if (_r) {                                                                    \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : expected failure: %s\n",      \
                                        __FILE__, __LINE__, #expr);                  \
            ++failures;                                                              \
        } else if (_r.Err().Code() != (code)) {                                       \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s got=%s want=%s msg=%.*s\n", \
                                        __FILE__, __LINE__, #expr,                    \
                                        ::mmo::core::ToString(_r.Err().Code()),        \
                                        ::mmo::core::ToString(code),                   \
                                        static_cast<int>(_r.Err().Message().size()),   \
                                        _r.Err().Message().data());                    \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

#define CHECK_OK(expr)                                                        \
    do {                                                                      \
        const auto _r = (expr);                                               \
        if (!_r) {                                                            \
            ::mmo::core::test::ErrorFmt(                                      \
                "FAIL @ %s:%d : unexpected failure: %s msg=%.*s\n", __FILE__, \
                __LINE__, #expr, ::mmo::core::ToString(_r.Err().Code()),      \
                static_cast<int>(_r.Err().Message().size()),                  \
                _r.Err().Message().data());                                   \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

// ---------------------------------------------------------------------------
// 探针：把脚本侧观察到的值回传给 C++ 断言（§7 的 Call 返回 Result<void>，无回传通道）
// ---------------------------------------------------------------------------

struct Probe {
    static constexpr std::size_t kCapacity = 128;
    std::int64_t values[kCapacity]{};
    std::size_t count{0};
    bool overflow{false};

    void Reset() {
        count = 0;
        overflow = false;
    }
    std::int64_t Last() const { return count > 0 ? values[count - 1] : 0; }
    std::int64_t At(std::size_t from_end) const {
        return count > from_end ? values[count - 1 - from_end] : 0;
    }
};

/// 测试宿主状态：同时被生产 VM 与隔离 VM 的绑定共享。
struct TestHost {
    Probe probe;
    /// 置 true 时 `probe.boom` 失败 —— 用于构造「校验通过、运行期才崩」的场景（§19 自动回滚）。
    bool boom{false};
    std::int64_t boom_calls{0};
    std::atomic<int> isolated_inits{0};
};

core::Result<int> ProbeReport(ScriptCall& call, void* user) {
    auto* probe = static_cast<Probe*>(user);
    if (probe == nullptr) {
        return core::Result<int>::Fail(core::Error(ErrorCode::INVALID_ARGUMENT, "probe not set",
                                                   core::domain::kLua));
    }
    if (probe->count >= Probe::kCapacity) {
        probe->overflow = true;
        return call.Done();
    }
    probe->values[probe->count] = call.ArgCount() > 0 ? call.Arg(0).AsInt() : -1;
    ++probe->count;
    return call.Done();
}

/// `probe.boom()` —— 由 `TestHost::boom` 决定成功或失败。冒烟函数用它来模拟运行期崩溃。
core::Result<int> ProbeBoom(ScriptCall& call, void* user) {
    auto* host = static_cast<TestHost*>(user);
    if (host == nullptr) {
        return core::Result<int>::Fail(core::Error(ErrorCode::INVALID_ARGUMENT, "host not set",
                                                   core::domain::kLua));
    }
    host->boom_calls += 1;
    if (host->boom) {
        return core::Result<int>::Fail(core::Error(ErrorCode::INTERNAL_ERROR,
                                                   "injected runtime crash", core::domain::kLua));
    }
    return call.Done();
}

/// `probe.reload_in_place(name, source)` —— 在**脚本执行中**调 ReloadInPlace，
/// 用于验证「Activate 时正在执行该脚本 → 拒绝，不打断当前调用」（§19 / §9）。
core::Result<int> ProbeReloadInPlace(ScriptCall& call, void* /*user*/) {
    if (call.ArgCount() != 2 || !call.ArgIsString(0) || !call.ArgIsString(1)) {
        return core::Result<int>::Fail(core::Error(ErrorCode::INVALID_ARGUMENT,
                                                   "probe.reload_in_place needs 2 strings",
                                                   core::domain::kLua));
    }
    const auto reloaded = call.Context().ReloadInPlace(call.Arg(0).AsStr(), call.Arg(1).AsStr());
    if (!reloaded) {
        return core::Result<int>::Fail(reloaded.Err());
    }
    call.SetResult(ScriptValue::Int(1));
    return call.Done();
}

void AddProbeBinding(ScriptContext& ctx, const char* name, mmo::script::NativeFn fn, void* user) {
    BindingDef def;
    def.name = name;
    def.kind = BindingKind::Native;
    def.fn = fn;
    def.user = user;
    def.read_only = true;
    def.description = "hot reload test probe";
    CHECK_OK(ctx.AddBinding(std::move(def)));
}

/// 隔离 VM 的准备回调：把与生产 VM **完全一致**的绑定面装进临时 VM。
core::Result<void> InstallTestBindings(ScriptContext& ctx, void* user) {
    auto* host = static_cast<TestHost*>(user);
    host->isolated_inits.fetch_add(1, std::memory_order_relaxed);
    AddProbeBinding(ctx, "probe.report", &ProbeReport, &host->probe);
    AddProbeBinding(ctx, "probe.boom", &ProbeBoom, host);
    AddProbeBinding(ctx, "probe.reload_in_place", &ProbeReloadInPlace, host);
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// 测试脚手架
// ---------------------------------------------------------------------------

/// 建立一个生产 VM（含探针绑定）+ 一个绑定好的 HotReloader。
struct Fixture {
    TestHost host;
    std::unique_ptr<ScriptContext> ctx_holder;
    std::unique_ptr<HotReloader> reloader;

    explicit Fixture(HotReloader::Config config = {}) {
        auto created = ScriptContext::Create(LuaLimits{});
        if (!created) {
            ::mmo::core::test::ErrorFmt("FATAL: ScriptContext::Create failed\n");
            ++failures;
            return;
        }
        ctx_holder = std::move(created).Value();
        // 生产 VM 也要装同样的绑定（隔离 VM 由 preparer 负责）。
        AddProbeBinding(*ctx_holder, "probe.report", &ProbeReport, &host.probe);
        AddProbeBinding(*ctx_holder, "probe.boom", &ProbeBoom, &host);
        AddProbeBinding(*ctx_holder, "probe.reload_in_place", &ProbeReloadInPlace, &host);
        reloader = std::make_unique<HotReloader>(*ctx_holder, config);
        reloader->SetIsolatedPreparer(&InstallTestBindings, &host);
    }

    ScriptContext& ctx() const { return *ctx_holder; }
    HotReloader& hr() const { return *reloader; }
    bool valid() const { return ctx_holder != nullptr && reloader != nullptr; }

    /// 跑一次完整流水线（Prepare → Validate → Activate），全程在安全点内激活。
    bool Publish(const char* name, std::string_view source, std::uint64_t tick = 1) {
        const auto prepared = hr().Prepare(name, source);
        if (!prepared) {
            ::mmo::core::test::ErrorFmt("Publish(%s): Prepare failed: %.*s\n", name,
                                        static_cast<int>(prepared.Err().Message().size()),
                                        prepared.Err().Message().data());
            ++failures;
            return false;
        }
        const auto report = hr().Validate(prepared.Value());
        if (!report) {
            ++failures;
            return false;
        }
        if (!report.Value().ok) {
            ::mmo::core::test::ErrorFmt("Publish(%s): validate not ok, issues=%zu\n", name,
                                        report.Value().issues.size());
            ++failures;
            return false;
        }
        hr().BeginSafePoint(tick);
        const auto activated = hr().Activate(prepared.Value(), core::kInvalidTraceId);
        hr().EndSafePoint();
        if (!activated) {
            ++failures;
            return false;
        }
        return true;
    }
};

constexpr const char* kScriptName = "damage_formula";

/// v1：每次调用计数 +1，报告 `counter * 10`。用来同时验证「行为」与「全局状态保留」。
constexpr const char* kV1 = R"LUA(
counter = counter or 0
function run()
  counter = counter + 1
  probe.report(counter * 10)
end
)LUA";

/// v2：同样的全局计数器，但报告 `counter * 100`（行为可区分）。
constexpr const char* kV2 = R"LUA(
counter = counter or 0
function run()
  counter = counter + 1
  probe.report(counter * 100)
end
)LUA";

/// v3：U 数学上等价于 v2，但报告 `counter * 1000`。
constexpr const char* kV3 = R"LUA(
counter = counter or 0
function run()
  counter = counter + 1
  probe.report(counter * 1000)
end
)LUA";

// ===========================================================================
// §16 六阶段状态机 + checksum
// ===========================================================================

void TestStateMachineAndChecksum() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    // 初始状态
    CHECK(fx.hr().StateOf(kScriptName) == ReloadState::Idle);
    CHECK(fx.hr().CurrentVersion(kScriptName) == nullptr);
    CHECK(fx.hr().History(kScriptName).empty());

    // 阶段 1-2：Prepare → Compiling 之后进入待校验
    const auto t1 = fx.hr().Prepare(kScriptName, kV1);
    CHECK_OK(t1);
    if (!t1) {
        return;
    }
    CHECK(t1.Value().compiled);
    CHECK(fx.hr().StateOf(kScriptName) == ReloadState::Validating);
    CHECK(!t1.Value().checksum.empty());
    CHECK(t1.Value().checksum.size() == 16u);

    // 阶段 3：Validate → PendingActivate
    const auto report = fx.hr().Validate(t1.Value());
    CHECK_OK(report);
    if (report) {
        CHECK(report.Value().ok);
        CHECK(report.Value().syntax_ok);
        CHECK(report.Value().sandbox_ok);
        CHECK(report.Value().smoke_ok);
        CHECK(report.Value().issues.empty());
    }
    CHECK(fx.hr().StateOf(kScriptName) == ReloadState::PendingActivate);
    CHECK(fx.hr().PendingCount() == 1u);

    // 未进安全点 → 不允许激活（§20 验收 #1）
    CHECK_FAIL_CODE(fx.hr().Activate(t1.Value(), core::kInvalidTraceId), ErrorCode::BUSY);
    CHECK(fx.hr().StateOf(kScriptName) == ReloadState::PendingActivate);

    // 阶段 4：安全点内激活
    fx.hr().BeginSafePoint(10);
    CHECK(fx.hr().InSafePoint());
    CHECK(fx.hr().SafePointTick() == 10u);
    CHECK_OK(fx.hr().Activate(t1.Value(), 4242));
    fx.hr().EndSafePoint();
    CHECK(!fx.hr().InSafePoint());
    CHECK(fx.hr().SafePointTick() == 0u);

    CHECK(fx.hr().StateOf(kScriptName) == ReloadState::Activated);
    CHECK(fx.hr().PendingCount() == 0u);
    CHECK(fx.hr().ActivatedCount() == 1u);

    const auto* current = fx.hr().CurrentVersion(kScriptName);
    CHECK(current != nullptr);
    if (current != nullptr) {
        CHECK(current->version == 1u);
        CHECK(current->checksum == t1.Value().checksum);
        CHECK(current->activated_at_ms > 0);
        // 操作者串里带上了 trace，便于审计回溯到具体请求
        CHECK(current->activated_by.find("trace=4242") != std::string::npos);
    }
    CHECK(fx.hr().ScriptCount() == 1u);

    // checksum：同源码同值、异源码异值、逐字节敏感
    const auto a = fx.hr().Prepare("cs_a", "x = 1\n");
    const auto b = fx.hr().Prepare("cs_b", "x = 1\n");
    const auto c = fx.hr().Prepare("cs_c", "x = 2\n");
    CHECK_OK(a);
    CHECK_OK(b);
    CHECK_OK(c);
    if (a && b && c) {
        CHECK(a.Value().checksum == b.Value().checksum);
        CHECK(a.Value().checksum != c.Value().checksum);
        CHECK(a.Value().checksum != t1.Value().checksum);
    }
}

// ===========================================================================
// §16 / §20 #1 Activate 只能在 Tick Safe Point；跨线程 → BUSY
// ===========================================================================

void TestActivateGateAndThreadModel() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    const auto prepared = fx.hr().Prepare(kScriptName, kV1);
    CHECK_OK(prepared);
    if (!prepared) {
        return;
    }
    CHECK_OK(fx.hr().Validate(prepared.Value()));

    // (1) 未校验的票证 → UNAUTHORIZED（§21 禁止未校验直接激活）
    mmo::script::ReloadTicket forged;
    forged.name = "forged_script";
    forged.source = kV1;
    forged.compiled = true;
    forged.report.ok = true;  // 伪造 report 也不放行（判据是内部记录，不是入参）
    fx.hr().BeginSafePoint(1);
    CHECK_FAIL_CODE(fx.hr().Activate(forged, core::kInvalidTraceId), ErrorCode::UNAUTHORIZED);

    // (2) 非 OwnerThread → BUSY（§9 / §20.1：不跨线程共享 VM）
    core::Result<void> other_thread_result = core::Result<void>::Ok();
    std::thread worker([&] {
        other_thread_result = fx.hr().Activate(prepared.Value(), core::kInvalidTraceId);
    });
    worker.join();
    CHECK(!other_thread_result);
    if (!other_thread_result) {
        CHECK(other_thread_result.Err().Code() == ErrorCode::BUSY);
    }
    fx.hr().EndSafePoint();

    // (3) 安全点关闭后仍拒绝
    CHECK_FAIL_CODE(fx.hr().Activate(prepared.Value(), core::kInvalidTraceId), ErrorCode::BUSY);

    // (4) Validate 是可以在别的线程跑的（Worker 线程模型）
    std::atomic<bool> off_thread_validate_ok{false};
    std::thread validator([&] {
        const auto t = fx.hr().Prepare("worker_script", "function f() return 1 end\n");
        if (t) {
            const auto r = fx.hr().Validate(t.Value());
            off_thread_validate_ok.store(r && r.Value().ok, std::memory_order_relaxed);
        }
    });
    validator.join();
    CHECK(off_thread_validate_ok.load(std::memory_order_relaxed));

    // 正常路径仍然可用
    fx.hr().BeginSafePoint(2);
    CHECK_OK(fx.hr().Activate(prepared.Value(), core::kInvalidTraceId));
    fx.hr().EndSafePoint();
    CHECK(fx.hr().ActivatedCount() == 1u);
}

// ===========================================================================
// §20 #2 编译失败 / 校验失败 → 线上旧版本完全不受影响
// ===========================================================================

void TestFailuresKeepOldVersionServing() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    if (!fx.Publish(kScriptName, kV1, /*tick=*/1)) {
        return;
    }
    const auto* before = fx.hr().CurrentVersion(kScriptName);
    CHECK(before != nullptr);
    const std::string baseline_checksum = before != nullptr ? before->checksum : std::string{};
    const std::uint32_t baseline_version = before != nullptr ? before->version : 0u;

    // 「旧版仍在服务」的判据：v1 的 `run()` 把全局 `counter` +1 后上报 `counter * 10`。
    //
    // 【踩坑 · 不能写死期望值】`counter` 是**脚本全局状态**，一次被拒的热更**不会**重置它 ——
    //   这恰是 §17「状态连续」与 §21「禁止半新半旧」的联合证据（旧版还在用、状态没被打断）。
    //   所以每次调用后期望值是 `++served * 10`。若写死 `== 10`，第 2 次调用会得到 20 而误判失败
    //   （实测：本用例最初就是因此报 `probe.Last() == 10` 失败）。
    std::int64_t served = 0;

    // ---- 语法错误 → Prepare 阶段即被拦下（INVALID_ARGUMENT）----
    CHECK_FAIL_CODE(fx.hr().Prepare(kScriptName, "function run( end\n"),
                     ErrorCode::INVALID_ARGUMENT);
    // 旧版本仍在服务，且版本号未变
    fx.host.probe.Reset();
    CHECK_OK(fx.ctx().Call(1, "run"));
    ++served;
    CHECK(fx.host.probe.Last() == served * 10);  // v1 行为（multiplier 恒为 10）
    CHECK(fx.hr().CurrentVersion(kScriptName)->version == baseline_version);
    CHECK(fx.hr().CurrentVersion(kScriptName)->checksum == baseline_checksum);

    // ---- 白名单外 API → Validate 拦下（sandbox_ok=false）----
    const auto banned = fx.hr().Prepare(kScriptName, "function run() io.open('x') end\n");
    CHECK_OK(banned);
    if (banned) {
        const auto report = fx.hr().Validate(banned.Value());
        CHECK_OK(report);
        if (report) {
            CHECK(!report.Value().ok);
            CHECK(!report.Value().sandbox_ok);
            CHECK(!report.Value().issues.empty());
        }
        // 校验未通过 → 安全点内激活也被拒绝，且旧版继续服务
        fx.hr().BeginSafePoint(3);
        CHECK_FAIL_CODE(fx.hr().Activate(banned.Value(), core::kInvalidTraceId),
                        ErrorCode::UNAUTHORIZED);
        fx.hr().EndSafePoint();
    }
    fx.host.probe.Reset();
    CHECK_OK(fx.ctx().Call(1, "run"));
    ++served;
    CHECK(fx.host.probe.Last() == served * 10);  // 仍是 v1 行为，且 counter 连续（没有被热更打断）
    CHECK(fx.hr().CurrentVersion(kScriptName)->checksum == baseline_checksum);
    CHECK(fx.hr().FailedCount() >= 1u);

    // ---- 顶层运行期错误 → Prepare 的隔离 VM 编译执行阶段拦下 ----
    CHECK_FAIL_CODE(fx.hr().Prepare("boom_top", "error('top level boom')\n"),
                     ErrorCode::INVALID_ARGUMENT);

    // ---- 静态扫描必须**剥离注释与字符串**：否则合法脚本会被误拒 ----
    const auto commented = fx.hr().Prepare(
        "commented", "local s = 'io os require'\n-- load the config from disk\nreturn s\n");
    CHECK_OK(commented);
    if (commented) {
        const auto report = fx.hr().Validate(commented.Value());
        CHECK_OK(report);
        if (report) {
            CHECK(report.Value().ok);  // 注释/字符串里的禁用词不算违规
        }
    }
}

// ===========================================================================
// §17 / §21 热替换：行为生效 + 脚本全局变量保留（ReloadInPlace 的核心语义）
// ===========================================================================

void TestReloadKeepsGlobalsAndChangesBehavior() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    if (!fx.Publish(kScriptName, kV1, 1)) {
        return;
    }
    const mmo::script::ScriptId id_v1 = fx.hr().CurrentVersion(kScriptName)->id;

    // v1：counter 累积到 2
    fx.host.probe.Reset();
    CHECK_OK(fx.ctx().Call(id_v1, "run"));
    CHECK_OK(fx.ctx().Call(id_v1, "run"));
    CHECK(fx.host.probe.count == 2u);
    CHECK(fx.host.probe.Last() == 20);  // counter=2 → 2*10

    // 热更到 v2：**id 必须不变**（下游持有的句柄继续有效）
    if (!fx.Publish(kScriptName, kV2, 2)) {
        return;
    }
    const auto* v2 = fx.hr().CurrentVersion(kScriptName);
    CHECK(v2 != nullptr);
    CHECK(v2->id == id_v1);
    CHECK(v2->version == 2u);
    CHECK(fx.hr().StateOf(kScriptName) == ReloadState::Activated);

    // 关键断言：counter 从 2 继续（全局状态保留），且新逻辑生效（*100 而非 *10）
    fx.host.probe.Reset();
    CHECK_OK(fx.ctx().Call(id_v1, "run"));
    CHECK(fx.host.probe.Last() == 300);  // counter=3 → 3*100
    // 再调一次确认是同一份 _ENV 在累积
    CHECK_OK(fx.ctx().Call(id_v1, "run"));
    CHECK(fx.host.probe.Last() == 400);  // counter=4 → 4*100
    CHECK(fx.ctx().LoadedCount() == 1u);  // 没有产生第二个脚本槽位
    CHECK(fx.ctx().Version() >= 2u);
}

// ===========================================================================
// §16 / §20 #5 回滚 + 版本历史截断
// ===========================================================================

void TestRollbackAndHistoryTruncation() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    // 回滚前：无上一版本 → NOT_FOUND
    fx.hr().BeginSafePoint(1);
    CHECK_FAIL_CODE(fx.hr().Rollback(kScriptName, core::kInvalidTraceId), ErrorCode::NOT_FOUND);
    fx.hr().EndSafePoint();

    if (!fx.Publish(kScriptName, kV1, 1)) {
        return;
    }
    if (!fx.Publish(kScriptName, kV2, 2)) {
        return;
    }
    // 回滚前再确认一次：只有 1 个历史版本时不可回滚
    fx.hr().BeginSafePoint(3);
    CHECK_OK(fx.hr().Rollback(kScriptName, core::kInvalidTraceId));
    fx.hr().EndSafePoint();

    CHECK(fx.hr().StateOf(kScriptName) == ReloadState::RolledBack);
    fx.host.probe.Reset();
    CHECK_OK(fx.ctx().Call(1, "run"));
    // 回到 v1 行为（*10）；且 counter 连续（前两次 publish 未调用 run，counter=0 → 1）
    CHECK(fx.host.probe.Last() == 10);

    // 回滚也是一次激活 → 记入历史（version=3）
    const auto history = fx.hr().History(kScriptName);
    CHECK(!history.empty());
    if (!history.empty()) {
        CHECK(history.front().version == 3u);
        CHECK(history.front().activated_by.find("rollback") != std::string::npos);
    }

    // 非安全点回滚 → BUSY
    CHECK_FAIL_CODE(fx.hr().Rollback(kScriptName, core::kInvalidTraceId), ErrorCode::BUSY);

    // ---- 连续多次热更：历史截断到 5 且内存有界（§19 / §21）----
    Fixture fx2;
    if (!fx2.valid()) {
        return;
    }
    if (!fx2.Publish(kScriptName, kV1, 1)) {
        return;
    }
    const std::size_t mem_after_first = fx2.ctx().MemoryUsed();
    for (std::uint64_t i = 2; i <= 12; ++i) {
        const char* src = (i % 2 == 0) ? kV2 : kV3;
        if (!fx2.Publish(kScriptName, src, i)) {
            return;
        }
    }
    const auto deep_history = fx2.hr().History(kScriptName);
    CHECK(deep_history.size() == 5u);  // §20 验收 #5：保留最近 5 个
    CHECK(fx2.hr().CurrentVersion(kScriptName)->version == 12u);
    CHECK(fx2.ctx().LoadedCount() == 1u);  // 反复热更没有堆积脚本槽位

    // 内存有界：12 次热更后相对第 1 次不应显著增长（真泄漏会线性累积）
    const std::size_t mem_after_twelve = fx2.ctx().MemoryUsed();
    ::mmo::core::test::LineFmt("  [info] memory: after_1_reload=%zu after_12_reloads=%zu\n",
                               mem_after_first, mem_after_twelve);
    CHECK(mem_after_twelve < mem_after_first + (256u * 1024u));
    CHECK(fx2.ctx().MemoryUsed() <= fx2.ctx().Vm().MemoryLimit());
}

// ===========================================================================
// §19 磁盘脚本文件：不存在 / 不可读 → NOT_FOUND
// ===========================================================================

void TestPrepareFromFile() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    // 文件不存在（含「热更前把脚本文件删了」的场景）
    CHECK_FAIL_CODE(fx.hr().PrepareFromFile("disk_script", "build/__no_such_script__.lua"),
                     ErrorCode::NOT_FOUND);

    // 正常读取
    const std::string path = "build/hotreload_test_script.lua";
    {
        std::ofstream out(path, std::ios::trunc);
        CHECK(out.good());
        out << "function run() probe.report(7) end\n";
    }
    const auto prepared = fx.hr().PrepareFromFile("disk_script", path);
    CHECK_OK(prepared);
    if (prepared) {
        CHECK(prepared.Value().origin == path);
        CHECK(prepared.Value().compiled);
    }

    // 关闭文件读取后应被拒（allow_filesystem_read 语义）
    HotReloader::Config no_io;
    no_io.allow_filesystem_read = false;
    HotReloader guarded(fx.ctx(), no_io);
    CHECK_FAIL_CODE(guarded.PrepareFromFile("disk_script2", path), ErrorCode::UNAUTHORIZED);
}

// ===========================================================================
// §19 / §20 #4 运行期崩溃 → 自动回滚
// ===========================================================================

void TestAutoRollbackOnRuntimeCrash() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    // 基线：一个好版本（含冒烟自检入口，调用成功）
    if (!fx.Publish(kScriptName, kV1, 1)) {
        return;
    }
    const std::string good_checksum = fx.hr().CurrentVersion(kScriptName)->checksum;

    // 坏版本：顶层与语法都合法（Validate 能过），**只有冒烟调用会崩**
    // 顶层不能崩（否则落进 Prepare 的拦截），所以崩溃点放在 __hot_smoke 里。
    constexpr const char* kCrashy = R"LUA(
counter = counter or 0
function run()
  counter = counter + 1
  probe.report(counter * 10)
end
function __hot_smoke()
  probe.boom()
end
)LUA";
    // 提交坏版本前 blast：让隔离 VM 的冒烟先通过
    fx.host.boom = false;
    if (!fx.Publish(kScriptName, kCrashy, 2)) {
        return;
    }
    CHECK(fx.hr().CurrentVersion(kScriptName)->version == 2u);
    CHECK(fx.hr().StateOf(kScriptName) == ReloadState::Activated);

    // 切换后注入崩溃：下一个安全点的 VerifyPass 应当自动回滚
    fx.host.boom = true;
    fx.hr().BeginSafePoint(3);
    const std::size_t verified = fx.hr().VerifyPass(core::kInvalidTraceId);
    fx.hr().EndSafePoint();
    CHECK(verified >= 1u);
    CHECK(fx.hr().AutoRollbackCount() == 1u);
    CHECK(fx.hr().StateOf(kScriptName) == ReloadState::RolledBack);

    // 回滚后旧版本继续服务，且行为回到 v1
    fx.host.boom = false;
    fx.host.probe.Reset();
    CHECK_OK(fx.ctx().Call(1, "run"));
    CHECK(fx.host.probe.Last() == 10);

    // 回滚的目标必须是那个好版本
    const auto history = fx.hr().History(kScriptName);
    bool found_good = false;
    for (const auto& v : history) {
        if (v.checksum == good_checksum) {
            found_good = true;
        }
    }
    CHECK(found_good);
    CHECK(fx.hr().CurrentVersion(kScriptName)->checksum == good_checksum);

    // 同一 Tick 内刚激活的脚本不在本 Tick 被验证（避免把切换瞬间当稳定态）
    Fixture fx2;
    if (!fx2.valid()) {
        return;
    }
    if (!fx2.Publish(kScriptName, kV1, 5)) {
        return;
    }
    fx2.hr().BeginSafePoint(5);  // 与激活同一 Tick
    CHECK(fx2.hr().VerifyPass(core::kInvalidTraceId) == 0u);
    fx2.hr().EndSafePoint();
}

// ===========================================================================
// §20 #6 审计记录 + 幂等落盘
// ===========================================================================

void TestAuditSink() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    const std::string path = "build/hotreload_test_audit.md";
    std::remove(path.c_str());  // 从干净状态开始

    MarkdownAuditSink sink(path);
    fx.hr().SetAuditSink(&sink);

    if (!fx.Publish(kScriptName, kV1, 1)) {
        return;
    }
    // 审计是「安全点内入队、Tick 外落盘」：未 Drain 前文件不应存在
    CHECK(fx.hr().DrainAudit() == 1u);
    CHECK(sink.Records() == 1u);
    CHECK(sink.Written() == 1u);

    if (!fx.Publish(kScriptName, kV2, 2)) {
        return;
    }
    CHECK(fx.hr().DrainAudit() == 1u);
    CHECK(sink.Records() == 2u);

    // 再 Drain 一次：队列已空 → 0，且文件内容不变（幂等）
    CHECK(fx.hr().DrainAudit() == 0u);
    CHECK(sink.Records() == 2u);
    const std::size_t written_before = sink.Written();
    CHECK(written_before == 2u);

    // 用同一份记录重建 sink：去重键命中 → 不重复写
    {
        MarkdownAuditSink replayed(path);
        CHECK(replayed.Records() == 2u);
        const auto history = fx.hr().History(kScriptName);
        CHECK(!history.empty());
        if (!history.empty()) {
            CHECK_OK(replayed.Append(history.back(), kScriptName));
            CHECK(replayed.Written() == 0u);
        }
    }

    // 无 sink 时 DrainAudit 返回 0 且记录保留在队列（不丢审计）
    Fixture fx3;
    if (!fx3.valid()) {
        return;
    }
    if (!fx3.Publish(kScriptName, kV1, 1)) {
        return;
    }
    CHECK(fx3.hr().DrainAudit() == 0u);
    fx3.hr().SetAuditSink(&sink);
    CHECK(fx3.hr().DrainAudit() == 1u);  // 装上 sink 后补写成功
}

// ===========================================================================
// §15-4 ActivatePending：安全点内批量激活
// ===========================================================================

void TestActivatePendingBatch() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        const std::string name = "batch_" + std::to_string(i);
        const auto t = fx.hr().Prepare(name, kV1);
        CHECK_OK(t);
        if (t) {
            const auto r = fx.hr().Validate(t.Value());
            CHECK_OK(r);
        }
    }
    CHECK(fx.hr().PendingCount() == 3u);
    CHECK(fx.hr().ScriptCount() == 3u);

    // 安全点外 → 不激活
    CHECK(fx.hr().ActivatePending(core::kInvalidTraceId) == 0u);
    CHECK(fx.hr().PendingCount() == 3u);

    fx.hr().BeginSafePoint(7);
    CHECK(fx.hr().ActivatePending(core::kInvalidTraceId) == 3u);
    fx.hr().EndSafePoint();

    CHECK(fx.hr().PendingCount() == 0u);
    CHECK(fx.hr().ActivatedCount() == 3u);
    for (int i = 0; i < 3; ++i) {
        const std::string name = "batch_" + std::to_string(i);
        CHECK(fx.hr().CurrentVersion(name) != nullptr);
        CHECK(fx.hr().StateOf(name) == ReloadState::Activated);
    }
}

// ===========================================================================
// ReloadInPlace 直接契约（TASK-032 加法扩展）
// ===========================================================================

void TestReloadInPlaceContract() {
    Fixture fx;
    if (!fx.valid()) {
        return;
    }
    // 未装载 → NOT_FOUND（热替换不是装载）
    CHECK_FAIL_CODE(fx.ctx().ReloadInPlace("never_loaded", "return 1"), ErrorCode::NOT_FOUND);

    if (!fx.Publish(kScriptName, kV1, 1)) {
        return;
    }
    // 空名字 → INVALID_ARGUMENT
    CHECK_FAIL_CODE(fx.ctx().ReloadInPlace("", "return 1"), ErrorCode::INVALID_ARGUMENT);

    // 先让 counter 累积到 1（v1 行为：报告 10）
    fx.host.probe.Reset();
    CHECK_OK(fx.ctx().Call(1, "run"));
    CHECK(fx.host.probe.Last() == 10);

    // 直接原地热替换（不经 HotReloader，验证 ScriptContext 这一层的契约）
    CHECK(!fx.ctx().InScriptExecution());
    CHECK_OK(fx.ctx().ReloadInPlace(kScriptName, kV3));

    // 一次调用同时证明两件事：counter 从 1 连续到 2（全局状态保留）+ v3 逻辑生效（*1000）
    fx.host.probe.Reset();
    CHECK_OK(fx.ctx().Call(1, "run"));
    CHECK(fx.host.probe.Last() == 2000);
    CHECK(fx.ctx().LoadedCount() == 1u);

    // ---- 脚本执行中热更 → BUSY（不打断当前调用，§9 / §19）----
    constexpr const char* kSelfReload = R"LUA(
function try_reload()
  probe.reload_in_place('self_reload', 'function try_reload() probe.report(-1) end\n')
end
)LUA";
    if (!fx.Publish("self_reload", kSelfReload, 2)) {
        return;
    }
    CHECK_FAIL_CODE(fx.ctx().Call(2, "try_reload"), ErrorCode::BUSY);
    // 执行区间已退出，此刻的原地替换恢复正常
    CHECK(!fx.ctx().InScriptExecution());
    CHECK_OK(fx.ctx().ReloadInPlace("self_reload", kSelfReload));
}

// ===========================================================================
// 状态名（观测接口不崩）
// ===========================================================================

void TestStateNames() {
    CHECK(std::string_view(ToString(ReloadState::Idle)) == "Idle");
    CHECK(std::string_view(ToString(ReloadState::Compiling)) == "Compiling");
    CHECK(std::string_view(ToString(ReloadState::Validating)) == "Validating");
    CHECK(std::string_view(ToString(ReloadState::PendingActivate)) == "PendingActivate");
    CHECK(std::string_view(ToString(ReloadState::Activated)) == "Activated");
    CHECK(std::string_view(ToString(ReloadState::RolledBack)) == "RolledBack");
    CHECK(std::string_view(ToString(ReloadState::Failed)) == "Failed");
    CHECK(std::string_view(ToString(static_cast<ReloadState>(250))) == "UNKNOWN");
}

}  // namespace

int main() {
    ::mmo::core::test::Line("=== TASK-032 Lua Hot Reload unit/failure tests ===");

    TestStateNames();
    TestStateMachineAndChecksum();
    TestActivateGateAndThreadModel();
    TestFailuresKeepOldVersionServing();
    TestReloadKeepsGlobalsAndChangesBehavior();
    TestRollbackAndHistoryTruncation();
    TestPrepareFromFile();
    TestAutoRollbackOnRuntimeCrash();
    TestAuditSink();
    TestActivatePendingBatch();
    TestReloadInPlaceContract();

    if (failures == 0) {
        ::mmo::core::test::Line("ALL HOT RELOAD TESTS PASSED");
        return 0;
    }
    ::mmo::core::test::ErrorFmt("HOT RELOAD TESTS FAILED: %d\n", failures);
    return 1;
}
