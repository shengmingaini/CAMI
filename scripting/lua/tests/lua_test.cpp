// scripting/lua/tests/lua_test.cpp —— TASK-031 · 单元测试（§16）+ 失败路径测试（§19）
//
// 自包含 harness（与 engine/core/tests 同口径）：纯断言 + 失败计数器，不依赖 gtest
// —— vcpkg 离线/计费失败时仍可完成验证（§28 本地可验证优先）。
// 输出统一走 test_print.h 的 fwrite 通道（红线禁止 std::cout / printf / std::cerr）。
//
// 覆盖清单（对应 §16 / §19 / §20 Acceptance Criteria）
//   §16  VM 创建销毁 · Load/Unload · 五项绑定 · 沙箱禁用项 · 四类限制触发与恢复 · 错误映射含行号
//   §19  死循环不卡 Tick · 内存超限后 VM 可继续 · 调用不存在 API · 脚本 error 宿主继续 ·
//        io/os 被拒 · 失效 EntityId → NOT_FOUND
//   §20.1 每 Scene 一个 VM、不跨线程共享（跨线程入口 → BUSY；并发双 VM 各自独立）
//   §13  Tick Safe Point：执行中热更暂存 / 限额中止回滚
//
// 脚本写法约定（重要）
// ------------------
// `Call(id, fn)` 在**脚本模块表**里查函数。模块表 = chunk 返回的 table，或（不返回时）
// 该脚本私有的 `_ENV`。因此测试脚本用**全局函数定义** `function go() ... end`
// （即写进 `_ENV`），而不是 `local function go()`（局部变量在 chunk 外不可见）。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include "mmo/core/bus/command_bus.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/bus/query_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_events.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/script/lua_vm.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"
#include "mmo/script/script_event.h"

#include "test_print.h"

using mmo::core::ErrorCode;
using mmo::script::BindingDef;
using mmo::script::BindingKind;
using mmo::script::LuaLimits;
using mmo::script::ScriptCall;
using mmo::script::ScriptContext;
using mmo::script::ScriptError;
using mmo::script::ScriptReply;
using mmo::script::ScriptValue;

namespace {

// 本文件在全局命名空间，`core::` 需要显式指向 mmo::core（mmo::script::core 不存在）。
namespace core = mmo::core;

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s\n", __FILE__, __LINE__,   \
                                        #cond);                                     \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

/// 期望失败并带指定错误码（错误码不符也判失败）。
/// 恒打印 `Err().Message()`：脚本层的真实错因（Lua 原始文本 + 行号）都在消息里，
/// 只打错误码会把「RuntimeError: attempt to call a table value」这类关键线索丢掉。
#define CHECK_FAIL_CODE(expr, code)                                                     \
    do {                                                                                \
        const auto _r = (expr);                                                         \
        if (_r) {                                                                       \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : expected failure: %s\n",         \
                                        __FILE__, __LINE__, #expr);                      \
            ++failures;                                                                 \
        } else if (_r.Err().Code() != (code)) {                                          \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s got=%s want=%s msg=%.*s\n",   \
                                        __FILE__, __LINE__, #expr,                       \
                                        ::mmo::core::ToString(_r.Err().Code()),          \
                                        ::mmo::core::ToString(code),                     \
                                        static_cast<int>(_r.Err().Message().size()),     \
                                        _r.Err().Message().data());                      \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_OK(expr)                                                                 \
    do {                                                                               \
        const auto _r = (expr);                                                        \
        if (!_r) {                                                                     \
            ::mmo::core::test::ErrorFmt(                                               \
                "FAIL @ %s:%d : unexpected failure: %s (%s) msg=%.*s\n", __FILE__,      \
                __LINE__, #expr, ::mmo::core::ToString(_r.Err().Code()),               \
                static_cast<int>(_r.Err().Message().size()),                           \
                _r.Err().Message().data());                                            \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// 探针绑定：把脚本侧观察到的值回传给 C++ 断言
//
// 为什么需要它：§7 冻结签名里 `Call` 返回 `Result<void>`（不回传脚本返回值），
// 因此测试必须有一条**受控回传通道**。用 §27.4 的注册表扩展点实现最贴切 ——
// 顺带验证「宿主可注册自己的绑定而不修改本模块任何文件」。
// ---------------------------------------------------------------------------
struct Probe {
    static constexpr std::size_t kCapacity = 64;
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

core::Result<int> ProbeReport(ScriptCall& call, void* user) {
    auto* probe = static_cast<Probe*>(user);
    if (probe == nullptr) {
        return core::Result<int>::Fail(
            mmo::core::Error(ErrorCode::INVALID_ARGUMENT, "probe not set", mmo::core::domain::kLua));
    }
    if (probe->count >= Probe::kCapacity) {
        probe->overflow = true;
        return call.Done();
    }
    probe->values[probe->count] = call.ArgCount() > 0 ? call.Arg(0).AsInt() : -1;
    ++probe->count;
    return call.Done();
}

/// `probe.hotload(name, source)` —— 在**脚本执行中**发起 Load，用于验证 §13 热更暂存。
core::Result<int> ProbeHotLoad(ScriptCall& call, void* /*user*/) {
    if (call.ArgCount() != 2 || !call.ArgIsString(0) || !call.ArgIsString(1)) {
        return core::Result<int>::Fail(core::Error(
            ErrorCode::INVALID_ARGUMENT, "probe.hotload needs 2 strings", mmo::core::domain::kLua));
    }
    const auto loaded = call.Context().Load(call.Arg(0).AsStr(), call.Arg(1).AsStr());
    if (!loaded) {
        return core::Result<int>::Fail(loaded.Err());
    }
    call.SetResult(ScriptValue::Int(static_cast<std::int64_t>(loaded.Value())));
    return call.Done();
}

void AddProbeBinding(ScriptContext& ctx, const char* name, mmo::script::NativeFn fn, void* user,
                     bool read_only) {
    BindingDef def;
    def.name = name;
    def.kind = BindingKind::Native;
    def.fn = fn;
    def.user = user;
    def.read_only = read_only;
    def.description = "test probe";
    CHECK_OK(ctx.AddBinding(std::move(def)));
}

// ---------------------------------------------------------------------------
// 宿主侧 op handler（模拟 SkillSystem / QuestSystem / Role 的 HP Owner）
// ---------------------------------------------------------------------------

struct HostState {
    std::int64_t last_arg0{0};
    std::int64_t last_arg1{0};
    std::int64_t op_calls{0};
    std::int64_t hp{1000};
};

core::Result<ScriptReply> HostCommandOp(const mmo::script::ScriptCommand& cmd, void* user) {
    auto* state = static_cast<HostState*>(user);
    state->op_calls += 1;
    state->last_arg0 = cmd.args.Size() > 0 ? cmd.args.At(0).AsInt() : -1;
    state->last_arg1 = cmd.args.Size() > 1 ? cmd.args.At(1).AsInt() : -1;

    if (cmd.Op() == "entity.set_hp") {
        // 唯一真正写 HP 的地方 —— 证明脚本只能「请求」，不能直接改（§8 / §20.5）。
        state->hp = state->last_arg1;
    }
    ScriptReply reply;
    reply.valid = true;
    reply.i0 = cmd.Op() == "skill.cast" ? 37 : state->hp;
    reply.i1 = static_cast<std::int64_t>(cmd.args.Size());
    reply.d0 = 1.5;
    return core::Result<ScriptReply>::Ok(reply);
}

core::Result<ScriptReply> HostQueryOp(const mmo::script::ScriptQuery& query, void* user) {
    auto* state = static_cast<HostState*>(user);
    state->op_calls += 1;
    const std::int64_t arg0 = query.args.Size() > 0 ? query.args.At(0).AsInt() : 0;
    ScriptReply reply;
    reply.valid = true;
    reply.i0 = arg0 * 2;
    return core::Result<ScriptReply>::Ok(reply);
}

// ===========================================================================
// §16 / §20.2 沙箱 + 绑定面自洽
// ===========================================================================

void TestSandboxAndBindingSurface() {
    Probe probe;
    auto created = ScriptContext::Create(LuaLimits{});
    CHECK(static_cast<bool>(created));
    if (!created) {
        return;
    }
    ScriptContext& ctx = *created.Value();
    AddProbeBinding(ctx, "probe.report", &ProbeReport, &probe, true);

    CHECK(!ctx.BindingsFrozen());
    CHECK(ctx.Bindings().Size() == 10u);  // entity×3 + skill×1 + quest×2 + event×2 + query×1 + probe
    CHECK(ctx.Bindings().SizeOf(BindingKind::Entity) == 3u);
    CHECK(ctx.Bindings().SizeOf(BindingKind::Skill) == 1u);
    CHECK(ctx.Bindings().SizeOf(BindingKind::Quest) == 2u);
    CHECK(ctx.Bindings().SizeOf(BindingKind::Event) == 2u);
    CHECK(ctx.Bindings().SizeOf(BindingKind::Query) == 1u);
    CHECK(ctx.Bindings().SizeOf(BindingKind::Native) == 1u);
    CHECK(ctx.Vm().SandboxOpen());
    CHECK(ctx.CommandOpCount() == 0u);
    CHECK(ctx.QueryOpCount() == 0u);

    // 名册冻结后禁止再改绑定表（防 Tick 中途行为漂移）
    ctx.FreezeBindings();
    CHECK(ctx.BindingsFrozen());
    BindingDef late;
    late.name = "probe.late";
    late.fn = &ProbeReport;
    CHECK_FAIL_CODE(ctx.AddBinding(std::move(late)), ErrorCode::BUSY);

    // ---- 沙箱：白名单内可用 / 白名单外一律不可达（§20.2）----
    static const char kSandboxScript[] = R"LUA(
local banned = {"io", "os", "require", "loadstring", "load", "loadfile", "dofile",
                "debug", "package", "coroutine", "collectgarbage", "warn", "module"}
local reachable = 0
for i = 1, #banned do
  if _G[banned[i]] ~= nil then reachable = reachable + 1 end
end
probe.report(reachable)                                     -- [0] 期望 0
probe.report(string.dump == nil and 1 or 0)                 -- [1] 期望 1
probe.report(("x").dump == nil and 1 or 0)                  -- [2] 期望 1（元表逃逸点已修）
probe.report(type(string.find) == "function" and 1 or 0)     -- [3] 期望 1
probe.report(type(table.insert) == "function" and 1 or 0)    -- [4] 期望 1
probe.report(type(math.floor) == "function" and 1 or 0)      -- [5] 期望 1
probe.report(math.randomseed == nil and 1 or 0)              -- [6] 期望 1（保 Replay 确定性）
probe.report(type(probe.report) == "function" and 1 or 0)    -- [7] 期望 1（宿主扩展点）
probe.report(_VERSION ~= nil and 1 or 0)                     -- [8] 期望 1
)LUA";
    const auto loaded = ctx.Load("sandbox.lua", kSandboxScript);
    CHECK_OK(loaded);
    if (loaded) {
        // 脚本没有 unused_fn：确认查询不到函数时返回明确错误（不是崩溃/静默成功）
        CHECK_FAIL_CODE(ctx.Call(loaded.Value(), "unused_fn"), ErrorCode::NOT_FOUND);
    }
    CHECK(!probe.overflow);
    CHECK(probe.count == 9u);
    if (probe.count == 9u) {
        CHECK(probe.values[0] == 0);
        for (std::size_t i = 1; i < 9; ++i) {
            CHECK(probe.values[i] == 1);
        }
    }

    // ---- VM 统计自洽 ----
    CHECK(ctx.MemoryUsed() > 0u);
    CHECK(ctx.MemoryUsed() <= ctx.Vm().MemoryLimit());
    CHECK(ctx.Vm().MemStats().peak >= ctx.Vm().MemStats().used);
    CHECK(ctx.Version() >= 1u);
    CHECK(ctx.Vm().OnOwnerThread());
}

// ===========================================================================
// §16 Load / Unload / 热替换 / 错误映射（含行号）
// ===========================================================================

void TestLoadUnloadAndErrorMapping() {
    Probe probe;
    auto created = ScriptContext::Create(LuaLimits{});
    CHECK(static_cast<bool>(created));
    if (!created) {
        return;
    }
    ScriptContext& ctx = *created.Value();
    AddProbeBinding(ctx, "probe.report", &ProbeReport, &probe, true);

    CHECK_FAIL_CODE(ctx.Load("", "return 1"), ErrorCode::INVALID_ARGUMENT);

    const auto a = ctx.Load("a.lua", "function hi() probe.report(11) end");
    CHECK_OK(a);
    CHECK(ctx.LoadedCount() == 1u);
    CHECK(ctx.IsLoaded(a.Value()));
    CHECK(ctx.NameOf(a.Value()) == "a.lua");
    CHECK_OK(ctx.Call(a.Value(), "hi"));
    CHECK(probe.Last() == 11);

    // 同名 Load = 热替换：id 不变（§13）
    const std::uint32_t version_before = ctx.Version();
    const auto a2 = ctx.Load("a.lua", "function hi() probe.report(22) end");
    CHECK_OK(a2);
    CHECK(a2.Value() == a.Value());
    CHECK(ctx.LoadedCount() == 1u);
    CHECK(ctx.Version() > version_before);
    CHECK_OK(ctx.Call(a.Value(), "hi"));
    CHECK(probe.Last() == 22);

    // 卸载 + 幂等（NOT_FOUND 可安全重试）
    CHECK_OK(ctx.Unload(a.Value()));
    CHECK(!ctx.IsLoaded(a.Value()));
    CHECK(ctx.LoadedCount() == 0u);
    CHECK_FAIL_CODE(ctx.Unload(a.Value()), ErrorCode::NOT_FOUND);
    CHECK_FAIL_CODE(ctx.Call(a.Value(), "hi"), ErrorCode::NOT_FOUND);

    // 返回 table ⇒ 模块表；不返回 ⇒ 私有 _ENV 作模块表
    const auto b = ctx.Load("b.lua", "local M = {}\nfunction M.go() probe.report(33) end\nreturn M");
    CHECK_OK(b);
    CHECK_OK(ctx.Call(b.Value(), "go"));
    CHECK(probe.Last() == 33);

    // 语法错误 → CompileError，**不留半成品**
    const std::uint32_t before = ctx.Version();
    const std::size_t loaded_before = ctx.LoadedCount();
    const auto broken = ctx.Load("broken.lua", "function ok()\n  local x = \nend");
    CHECK(!broken);
    CHECK(broken.Err().Code() == ErrorCode::INVALID_ARGUMENT);  // CompileError → INVALID_ARGUMENT
    CHECK(ctx.LastError().code == ScriptError::CompileError);
    CHECK(ctx.Version() == before);
    CHECK(ctx.LoadedCount() == loaded_before);
    CHECK(ctx.LastError().line > 0);                     // §15.9 必须带行号
    CHECK(ctx.LastError().script == "broken.lua");
    CHECK(broken.Err().Message().find("broken.lua:") != std::string_view::npos);

    // 编译失败后 VM 仍可用
    const auto c = ctx.Load("c.lua", "function ok2() probe.report(44) end");
    CHECK_OK(c);
    CHECK_OK(ctx.Call(c.Value(), "ok2"));
    CHECK(probe.Last() == 44);

    // 预编译字节码被拒（mode "t"）
    const auto bc = ctx.Load("bc.lua", std::string_view("\x1bLua", 4));
    CHECK(!bc);
    CHECK(ctx.LastError().code == ScriptError::CompileError);

    // 运行期 error 被捕获，宿主继续（§19 / §20.4）
    const auto e = ctx.Load("err.lua", "function boom()\n  error('kaboom')\nend\n"
                                      "function fine() probe.report(303) end");
    CHECK_OK(e);
    const auto bad = ctx.Call(e.Value(), "boom");
    CHECK(!bad);
    CHECK(bad.Err().Code() == ErrorCode::INTERNAL_ERROR);  // RuntimeError → INTERNAL_ERROR
    CHECK(ctx.LastError().code == ScriptError::RuntimeError);
    CHECK(ctx.LastError().line == 2);
    CHECK(ctx.LastError().script == "err.lua");
    CHECK(bad.Err().Message().find("err.lua:2") != std::string_view::npos);
    CHECK_OK(ctx.Call(e.Value(), "fine"));
    CHECK(probe.Last() == 303);

    // 索引 nil 也是 RuntimeError，不崩溃
    const auto n = ctx.Load("nil.lua", "function go_nil() local x = nil return x.y end");
    CHECK_OK(n);
    const auto nil_result = ctx.Call(n.Value(), "go_nil");
    CHECK(!nil_result);
    CHECK(ctx.LastError().code == ScriptError::RuntimeError);

    // 调用不存在的 API → 明确错误而非崩溃（§19）
    const auto api = ctx.Load("api.lua", "function go_api() nosuch.api(1) end");
    CHECK_OK(api);
    const auto api_result = ctx.Call(api.Value(), "go_api");
    CHECK(!api_result);
    CHECK(ctx.LastError().code == ScriptError::RuntimeError);
    CHECK(api_result.Err().Message().find("api.lua:1") != std::string_view::npos);

    // 参数过多 → INVALID_ARGUMENT（禁止静默截断）
    CHECK_FAIL_CODE(ctx.Call(api.Value(), "go_api", 1, 2, 3, 4, 5, 6, 7, 8, 9),
                    ErrorCode::INVALID_ARGUMENT);

    // 脚本全局隔离：两个脚本的 _ENV 不互相污染（§4 单 Owner 精神）
    const auto s1 = ctx.Load("s1.lua", "function put() shared = 777 end");
    const auto s2 = ctx.Load("s2.lua", "function peek() probe.report(shared == nil and 1 or 0) end");
    CHECK_OK(s1);
    CHECK_OK(s2);
    CHECK_OK(ctx.Call(s1.Value(), "put"));
    CHECK_OK(ctx.Call(s2.Value(), "peek"));
    CHECK(probe.Last() == 1);
}

// ===========================================================================
// §16 / §19 四类限制：触发正确 + 触发后 VM 仍可用
// ===========================================================================

void TestFourLimits() {
    Probe probe;

    // ---- 1) 指令上限：死循环不卡死宿主（§19）----
    {
        LuaLimits limits;
        limits.max_instructions = 200000;
        limits.max_exec_time = mmo::core::DurationMs{5000};
        auto created = ScriptContext::Create(limits);
        CHECK(static_cast<bool>(created));
        if (!created) {
            return;
        }
        ScriptContext& ctx = *created.Value();
        AddProbeBinding(ctx, "probe.report", &ProbeReport, &probe, true);

        const auto id = ctx.Load("spin.lua", "function spin() while true do end end");
        CHECK_OK(id);
        const auto result = ctx.Call(id.Value(), "spin");
        CHECK(!result);
        CHECK(result.Err().Code() == ErrorCode::TIMEOUT);  // InstructionLimit → TIMEOUT
        CHECK(ctx.LastError().code == ScriptError::InstructionLimit);
        CHECK(ctx.LastError().script == "spin.lua");
        CHECK(ctx.LimitHits() >= 1u);

        // 触发后 VM 可继续跑其它脚本（预算不复用，§19）
        const std::size_t hits = ctx.LimitHits();
        const auto ok_id = ctx.Load("after.lua", "function go_after() probe.report(101) end");
        CHECK_OK(ok_id);
        CHECK_OK(ctx.Call(ok_id.Value(), "go_after"));
        CHECK(probe.Last() == 101);
        CHECK(ctx.LimitHits() == hits);
    }

    // ---- 2) 时间上限（与指令上限双保险，§15.5）----
    {
        LuaLimits limits;
        limits.max_instructions = 4000000000u;  // 远大于可跑完的量：确保挂钟先到
        limits.max_exec_time = mmo::core::DurationMs{1};
        auto created = ScriptContext::Create(limits);
        CHECK(static_cast<bool>(created));
        if (!created) {
            return;
        }
        ScriptContext& ctx = *created.Value();
        const auto id = ctx.Load("timeout.lua", "function spin2() while true do end end");
        CHECK_OK(id);
        const auto result = ctx.Call(id.Value(), "spin2");
        CHECK(!result);
        CHECK(result.Err().Code() == ErrorCode::TIMEOUT);
        CHECK(ctx.LastError().code == ScriptError::Timeout);
    }

    // ---- 3) 栈深上限：防无限递归打爆 C 栈 ----
    // 注意脚本必须写成**非尾调用**（`1 + f(...)`）：Lua 会优化尾调用，
    // 写成 `return f(n+1)` 就不会增长栈，限制也就测不出来。
    {
        LuaLimits limits;
        limits.max_stack_depth = 32;
        auto created = ScriptContext::Create(limits);
        CHECK(static_cast<bool>(created));
        if (!created) {
            return;
        }
        ScriptContext& ctx = *created.Value();
        const auto id = ctx.Load("rec.lua", "function rec(n) return 1 + rec(n + 1) end");
        CHECK_OK(id);
        const auto result = ctx.Call(id.Value(), "rec", 1);
        CHECK(!result);
        CHECK(result.Err().Code() == ErrorCode::INTERNAL_ERROR);  // StackOverflow → INTERNAL_ERROR
        CHECK(ctx.LastError().code == ScriptError::StackOverflow);
        CHECK(ctx.LimitHits() >= 1u);
    }

    // ---- 4) 内存上限：分配失败 → MemoryLimit，VM 可继续跑其它脚本（§19）----
    {
        LuaLimits limits;
        limits.memory_bytes = 1024u * 1024u;
        auto created = ScriptContext::Create(limits);
        CHECK(static_cast<bool>(created));
        if (!created) {
            return;
        }
        ScriptContext& ctx = *created.Value();
        AddProbeBinding(ctx, "probe.report", &ProbeReport, &probe, true);

        const auto id = ctx.Load(
            "hog.lua", "function hog() local t = {} for i = 1, 500000 do t[i] = i end end");
        CHECK_OK(id);
        const auto result = ctx.Call(id.Value(), "hog");
        CHECK(!result);
        CHECK(ctx.LastError().code == ScriptError::MemoryLimit);
        CHECK(ctx.Vm().AllocFailCalls() >= 1u);
        CHECK(ctx.MemoryUsed() <= limits.memory_bytes);

        const auto ok_id = ctx.Load("small.lua", "function go_small() probe.report(202) end");
        CHECK_OK(ok_id);
        CHECK_OK(ctx.Call(ok_id.Value(), "go_small"));
        CHECK(probe.Last() == 202);
    }

    // ---- 5) 非法 limits 一律拒绝 ----
    {
        LuaLimits bad_mem;
        bad_mem.memory_bytes = 0;
        CHECK(!ScriptContext::Create(bad_mem));
        LuaLimits bad_time;
        bad_time.max_exec_time = mmo::core::DurationMs{0};
        CHECK(!ScriptContext::Create(bad_time));
        LuaLimits bad_instr;
        bad_instr.max_instructions = 0;
        CHECK(!ScriptContext::Create(bad_instr));
        LuaLimits bad_stack;
        bad_stack.max_stack_depth = 0;
        CHECK(!ScriptContext::Create(bad_stack));
    }
}

// ===========================================================================
// §16 / §20.5 五项绑定：Entity / Skill / Quest / Event / Query
// ===========================================================================

void TestFiveBindings() {
    Probe probe;
    HostState host;
    mmo::core::EventBus bus;
    mmo::core::CommandBus commands;
    mmo::core::QueryBus queries;

    auto created = ScriptContext::Create(LuaLimits{});
    CHECK(static_cast<bool>(created));
    if (!created) {
        return;
    }
    ScriptContext& ctx = *created.Value();
    AddProbeBinding(ctx, "probe.report", &ProbeReport, &probe, true);

    mmo::game::EntityManager entities(&bus, 1024);

    // 未绑定上游系统 → 明确错误（不是崩溃，§19）
    const auto unbound = ctx.Load("unbound.lua", "function go() entity.get(1) end\n"
                                                "function go2() skill.cast(1, 2, 3) end\n"
                                                "function go3() query.ask('x', 1) end\n"
                                                "function go4() event.publish('x', 1) end");
    CHECK_OK(unbound);
    CHECK_FAIL_CODE(ctx.Call(unbound.Value(), "go"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(unbound.Value(), "go2"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(unbound.Value(), "go3"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(unbound.Value(), "go4"), ErrorCode::INVALID_ARGUMENT);

    CHECK_OK(ctx.BindEntityApi(entities));
    CHECK_OK(ctx.BindCommandApi(commands));
    CHECK_OK(ctx.BindQueryApi(queries));
    CHECK_OK(ctx.BindEventApi(bus));

    CHECK_OK(ctx.RegisterCommandOp("entity.set_hp", &HostCommandOp, &host));
    CHECK_OK(ctx.RegisterCommandOp("skill.cast", &HostCommandOp, &host));
    CHECK_OK(ctx.RegisterCommandOp("quest.set_progress", &HostCommandOp, &host));
    CHECK_OK(ctx.RegisterCommandOp("quest.complete", &HostCommandOp, &host));
    CHECK_OK(ctx.RegisterQueryOp("probe.query", &HostQueryOp, &host));
    CHECK(ctx.CommandOpCount() == 4u);
    CHECK(ctx.QueryOpCount() == 1u);
    // 重名 op 拒绝（禁止静默覆盖）
    CHECK_FAIL_CODE(ctx.RegisterCommandOp("skill.cast", &HostCommandOp, &host),
                    ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.RegisterQueryOp("", &HostQueryOp, &host), ErrorCode::INVALID_ARGUMENT);

    mmo::game::Position pos;
    pos.x = 12.5f;
    pos.y = -3.25f;
    pos.z = 7.0f;
    pos.yaw = 1.0f;
    const auto made = entities.Create(mmo::game::EntityType::Monster, 9u, pos);
    CHECK_OK(made);
    if (!made) {
        return;
    }
    const mmo::game::EntityId eid = made.Value()->Id();

    // ---- Entity 只读面 ----
    const auto ent = ctx.Load("ent.lua", R"LUA(
function inspect(id)
  local e = entity.get(id)
  probe.report(e.id == id and 1 or 0)
  probe.report(e.type)
  probe.report(e.scene)
  probe.report(e.alive and 1 or 0)
  local p = entity.get_pos(id)
  probe.report(math.floor(p.x))
  probe.report(math.floor(p.y))
  probe.report(math.floor(p.z))
end
)LUA");
    CHECK_OK(ent);
    probe.Reset();
    CHECK_OK(ctx.Call(ent.Value(), "inspect", static_cast<std::int64_t>(eid)));
    CHECK(probe.count == 7u);
    if (probe.count == 7u) {
        CHECK(probe.values[0] == 1);
        CHECK(probe.values[1] == 2);   // Monster
        CHECK(probe.values[2] == 9);   // scene
        CHECK(probe.values[3] == 1);   // alive
        CHECK(probe.values[4] == 12);  // floor(12.5)
        CHECK(probe.values[5] == -4);  // floor(-3.25)
        CHECK(probe.values[6] == 7);
    }

    // 失效 / 非法 id → 明确错误（§19 / §20.1）
    const auto gone = ctx.Load("gone.lua", "function go(id) local e = entity.get(id) end");
    CHECK_OK(gone);
    CHECK_FAIL_CODE(ctx.Call(gone.Value(), "go", 999999), ErrorCode::NOT_FOUND);
    CHECK_FAIL_CODE(ctx.Call(gone.Value(), "go", 0), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(gone.Value(), "go", "abc"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(gone.Value(), "go"), ErrorCode::INVALID_ARGUMENT);

    // ---- entity.set_hp：受控写（只有宿主 op handler 真的改了 HP）----
    const auto set_id =
        ctx.Load("sethp.lua", "function set(id, v)\n"
                              "  local r = entity.set_hp(id, v)\n"
                              "  probe.report(r.ok and 1 or 0)\n"
                              "  probe.report(r.i0)\n"
                              "end");
    CHECK_OK(set_id);
    probe.Reset();
    const std::int64_t hp_before = host.hp;
    CHECK_OK(ctx.Call(set_id.Value(), "set", static_cast<std::int64_t>(eid), 250));
    CHECK(host.hp == 250);
    CHECK(host.hp != hp_before);
    CHECK(probe.count == 2u);
    CHECK(probe.values[0] == 1);
    CHECK(probe.values[1] == 250);
    // 参数个数 / 类型错误
    CHECK_FAIL_CODE(ctx.Call(set_id.Value(), "set", 1), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(set_id.Value(), "set", 1, 2.5), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(set_id.Value(), "set", 999999, 1), ErrorCode::NOT_FOUND);

    // ---- Skill / Quest：经 CommandBus 派发 ----
    // 注意：arity 校验必须用**能改变实参个数**的包装函数来测。
    // 若包装函数写成 `function cast(a,b,c) skill.cast(a,b,c) end`，则 Lua 侧
    // `cast(1,2)` 传给 skill.cast 的仍是 3 个值（第三个为 nil），
    // `cast(1,2,3,4)` 的第四个值被形参截断 —— 两种情况都够 3 个，
    // 断言永远不可能失败（这是个「恒真测试」，比没有测试更危险）。
    const auto skill = ctx.Load("skill.lua", "function cast(a, b, c)\n"
                                            "  local r = skill.cast(a, b, c)\n"
                                            "  probe.report(r.ok and 1 or 0)\n"
                                            "  probe.report(r.i0)\n"
                                            "  probe.report(r.i1)\n"
                                            "end\n"
                                            "function cast2(a, b) skill.cast(a, b) end\n"
                                            "function cast4(a, b, c, d) skill.cast(a, b, c, d) end");
    CHECK_OK(skill);
    probe.Reset();
    host.op_calls = 0;
    CHECK_OK(ctx.Call(skill.Value(), "cast", 1, 2, 3));
    CHECK(host.op_calls == 1);
    CHECK(host.last_arg0 == 1 && host.last_arg1 == 2);
    CHECK(probe.count == 3u);
    CHECK(probe.values[0] == 1);
    CHECK(probe.values[1] == 37);  // handler 算出的伤害，脚本无法干预
    CHECK(probe.values[2] == 3);   // 载荷参数个数
    CHECK_FAIL_CODE(ctx.Call(skill.Value(), "cast2", 1, 2), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(skill.Value(), "cast4", 1, 2, 3, 4), ErrorCode::INVALID_ARGUMENT);

    const auto quest = ctx.Load("quest.lua", "function sp(a, b, c)\n"
                                            "  local r = quest.set_progress(a, b, c)\n"
                                            "  probe.report(r.ok and 1 or 0)\n"
                                            "end\n"
                                            "function done(a, b)\n"
                                            "  local r = quest.complete(a, b)\n"
                                            "  probe.report(r.ok and 1 or 0)\n"
                                            "end\n"
                                            "function done1(a) quest.complete(a) end");
    CHECK_OK(quest);
    CHECK_OK(ctx.Call(quest.Value(), "sp", 1, 2, 3));
    CHECK(probe.Last() == 1);
    CHECK_OK(ctx.Call(quest.Value(), "done", 1, 2));
    CHECK(probe.Last() == 1);
    CHECK_FAIL_CODE(ctx.Call(quest.Value(), "done1", 1), ErrorCode::INVALID_ARGUMENT);

    // ---- Query：只读通道 ----
    const auto q = ctx.Load("q.lua", "function ask(v)\n"
                                    "  local r = query.ask('probe.query', v)\n"
                                    "  probe.report(r.ok and 1 or 0)\n"
                                    "  probe.report(r.i0)\n"
                                    "end\n"
                                    "function nope() query.ask('nope.op', 1) end\n"
                                    "function bad1() query.ask() end\n"
                                    "function bad2() query.ask(7, 1) end");
    CHECK_OK(q);
    probe.Reset();
    CHECK_OK(ctx.Call(q.Value(), "ask", 21));
    CHECK(probe.count == 2u);
    CHECK(probe.values[0] == 1);
    CHECK(probe.values[1] == 42);
    CHECK_FAIL_CODE(ctx.Call(q.Value(), "nope"), ErrorCode::NOT_FOUND);
    CHECK_FAIL_CODE(ctx.Call(q.Value(), "bad1"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(q.Value(), "bad2"), ErrorCode::INVALID_ARGUMENT);

    // ---- 绑定契约自洽：只读标记 + 分类 ----
    const auto qdef = ctx.Bindings().Find("query.ask");
    CHECK(static_cast<bool>(qdef));
    if (qdef) {
        CHECK(qdef.Value()->read_only);
        CHECK(qdef.Value()->kind == BindingKind::Query);
    }
    const auto hpdef = ctx.Bindings().Find("entity.set_hp");
    CHECK(static_cast<bool>(hpdef));
    if (hpdef) {
        CHECK(!hpdef.Value()->read_only);
    }
    const auto getdef = ctx.Bindings().Find("entity.get");
    CHECK(static_cast<bool>(getdef));
    if (getdef) {
        CHECK(getdef.Value()->read_only);
    }
    CHECK(!ctx.Bindings().Contains("no.such.binding"));

    // ---- 销毁后旧 id 立刻失效（世代 +1 → 防 ABA）----
    CHECK_OK(entities.Destroy(eid));
    CHECK_FAIL_CODE(ctx.Call(gone.Value(), "go", static_cast<std::int64_t>(eid)),
                    ErrorCode::NOT_FOUND);
    CHECK_FAIL_CODE(ctx.Call(set_id.Value(), "set", static_cast<std::int64_t>(eid), 1),
                    ErrorCode::NOT_FOUND);
    entities.FlushDeferred();
}

// ===========================================================================
// §17 事件链路（单元级）：C++ 发事件 → Lua 订阅者 → 脚本发布 → 脚本订阅者
// ===========================================================================

void TestEventBindings() {
    Probe probe;
    mmo::core::EventBus bus;
    mmo::core::CommandBus commands;

    auto created = ScriptContext::Create(LuaLimits{});
    CHECK(static_cast<bool>(created));
    if (!created) {
        return;
    }
    ScriptContext& ctx = *created.Value();
    AddProbeBinding(ctx, "probe.report", &ProbeReport, &probe, true);
    CHECK_OK(ctx.BindEventApi(bus));
    CHECK_OK(ctx.BindCommandApi(commands));

    mmo::game::EntityManager entities(&bus, 1024);
    CHECK_OK(ctx.BindEntityApi(entities));

    // ---- 实体生命周期事件桥 ----
    const auto sub = ctx.Load("sub.lua", "function on_created(t) probe.report(t.id) end\n"
                                        "event.subscribe('entity.created', on_created)");
    CHECK_OK(sub);
    CHECK(ctx.SubscriberCount("entity.created") == 1u);
    CHECK(ctx.SubscriberCount("never.subscribed") == 0u);

    // 订阅参数非法 → 明确错误
    const auto badsub = ctx.Load(
        "badsub.lua",
        "function a() event.subscribe('entity.created') end\n"
        "function b() event.subscribe(7, function() end) end\n"
        "function c() event.subscribe('entity.created', 5) end\n"
        "function d() event.subscribe(string.rep('z', 40), function() end) end");
    CHECK_OK(badsub);
    CHECK_FAIL_CODE(ctx.Call(badsub.Value(), "a"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(badsub.Value(), "b"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(badsub.Value(), "c"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(badsub.Value(), "d"), ErrorCode::INVALID_ARGUMENT);
    CHECK(ctx.SubscriberCount("entity.created") == 1u);

    // C++ 造实体 → 入队 → Drain 派发 → Lua 回调收到
    mmo::game::Position pos;
    const auto made = entities.Create(mmo::game::EntityType::Npc, 3u, pos);
    CHECK_OK(made);
    if (!made) {
        return;
    }
    CHECK(bus.QueueDepth() >= 1u);
    probe.Reset();
    CHECK_OK(bus.Drain(4096, mmo::core::DurationMs{50}));
    CHECK(probe.count == 1u);
    if (probe.count == 1u) {
        CHECK(probe.values[0] == static_cast<std::int64_t>(made.Value()->Id()));
    }

    // 组件挂载事件桥
    struct Dummy : mmo::game::Component<Dummy> {
        int v{0};
    };
    const auto csub = ctx.Load("csub.lua", "function on_comp(t) probe.report(t.id) end\n"
                                          "event.subscribe('component.attached', on_comp)");
    CHECK_OK(csub);
    CHECK(ctx.SubscriberCount("component.attached") == 1u);
    CHECK(made.Value()->AddComponent<Dummy>() != nullptr);
    probe.Reset();
    CHECK_OK(bus.Drain(4096, mmo::core::DurationMs{50}));
    CHECK(probe.count >= 1u);

    // ---- event.publish：脚本发布 → 脚本订阅者（经真实 EventBus，**异步**）----
    const auto pub = ctx.Load(
        "pub.lua",
        "function on_custom(t) probe.report(t[1]) probe.report(t[2]) probe.report(t[3]) end\n"
        "event.subscribe('probe.custom', on_custom)\n"
        "function fire() event.publish('probe.custom', 7, 'hi', 1.5) end");
    CHECK_OK(pub);
    CHECK(ctx.SubscriberCount("probe.custom") == 1u);
    probe.Reset();
    CHECK_OK(ctx.Call(pub.Value(), "fire"));
    CHECK(bus.QueueDepth() >= 1u);  // 发布只入队
    CHECK(probe.count == 0u);       // 尚未 Drain → 订阅者还没被调用
    CHECK_OK(bus.Drain(4096, mmo::core::DurationMs{50}));
    CHECK(probe.count == 3u);
    if (probe.count == 3u) {
        CHECK(probe.values[0] == 7);
        CHECK(probe.values[1] == 0);  // 'hi' 非整数 → AsInt()=0（位置语义见 INTERFACE.md §5）
        CHECK(probe.values[2] == 1);  // 1.5 截断
    }

    // publish 参数非法 → 拒绝（禁止截断 / 禁止 nil 打洞）
    const auto badpub = ctx.Load(
        "badpub.lua",
        "function a() event.publish('probe.custom') end\n"
        "function b() event.publish(7) end\n"
        "function c() event.publish('probe.custom', 1, 2, 3, 4, 5) end\n"
        "function d() event.publish('probe.custom', nil) end\n"
        "function e() event.publish(string.rep('z', 40)) end");
    CHECK_OK(badpub);
    CHECK_OK(ctx.Call(badpub.Value(), "a"));  // 0 个载荷参数合法
    CHECK_FAIL_CODE(ctx.Call(badpub.Value(), "b"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(badpub.Value(), "c"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(badpub.Value(), "d"), ErrorCode::INVALID_ARGUMENT);
    CHECK_FAIL_CODE(ctx.Call(badpub.Value(), "e"), ErrorCode::INVALID_ARGUMENT);
    CHECK_OK(bus.Drain(4096, mmo::core::DurationMs{50}));

    // ---- 派发隔离：一个订阅者抛错，另一个照常收到（§19）----
    const auto iso = ctx.Load(
        "iso.lua",
        "function boom(t) error('subscriber blew up') end\n"
        "function good(t) probe.report(t[1]) end\n"
        "event.subscribe('probe.iso', boom)\n"
        "event.subscribe('probe.iso', good)\n"
        "function fire_iso() event.publish('probe.iso', 99) end");
    CHECK_OK(iso);
    CHECK(ctx.SubscriberCount("probe.iso") == 2u);
    probe.Reset();
    CHECK_OK(ctx.Call(iso.Value(), "fire_iso"));
    CHECK_OK(bus.Drain(4096, mmo::core::DurationMs{50}));
    CHECK(probe.count == 1u);
    if (probe.count == 1u) {
        CHECK(probe.values[0] == 99);
    }

    // ---- 订阅者死循环：不卡死 Drain（限额在订阅者调用内生效）----
    //
    // 限额必须显式设置：默认 `max_instructions = 10M / max_exec_time = 5ms`
    // 对死循环而言 **挂钟先到**（10M 指令远跑不进 5ms），断言 InstructionLimit 就会
    // 随机变成 Timeout。这里把指令预算压小、挂钟放宽，让「指令上限」确定性地先触发
    // —— 与 §16「四类限制」用例 1 同一口径。
    {
        Probe slow;
        LuaLimits limits;
        limits.max_instructions = 200000;
        limits.max_exec_time = mmo::core::DurationMs{5000};
        // 【生命周期红线】总线必须比绑定它的 ScriptContext **长寿**：
        // `~ScriptContext` 会向 `services_.events` 退订自己安装的桥接订阅，
        // 若总线先析构，这一步就是 use-after-free（实测崩溃位置：EventBus::Unsubscribe）。
        // C++ 局部对象按声明逆序析构，所以总线必须声明在上下文**之前**。
        // 早前这里把 `local_bus` 写在 `if` 块里、`lone` 写在外层，
        // 导致 local_bus 先析构、lone 后析构 → 段错误。
        mmo::core::EventBus local_bus;
        auto lone = ScriptContext::Create(limits);
        CHECK(static_cast<bool>(lone));
        if (lone) {
            AddProbeBinding(*lone.Value(), "probe.report", &ProbeReport, &slow, true);
            CHECK_OK(lone.Value()->BindEventApi(local_bus));
            const auto s = lone.Value()->Load(
                "spin.lua",
                "function spin_sub(t) while true do end end\n"
                "event.subscribe('probe.spin', spin_sub)\n"
                "function fire() event.publish('probe.spin', 1) end\n"
                "function ok_sub(t) probe.report(1) end\n"
                "event.subscribe('probe.spin', ok_sub)");
            CHECK_OK(s);
            CHECK_OK(lone.Value()->Call(s.Value(), "fire"));
            const auto drain = local_bus.Drain(4096, mmo::core::DurationMs{50});
            CHECK_OK(drain);  // Drain 本身必须成功返回（限额把它打断了）
            CHECK(slow.count == 1u);
            CHECK(lone.Value()->LastError().code == ScriptError::InstructionLimit);
            CHECK(lone.Value()->LimitHits() >= 1u);
        }
    }

    // 未绑定 EventBus 时 publish 报明确错误
    {
        auto lone = ScriptContext::Create(LuaLimits{});
        CHECK(static_cast<bool>(lone));
        if (lone) {
            const auto p = lone.Value()->Load("lone.lua",
                                              "function go() event.publish('x', 1) end");
            CHECK_OK(p);
            CHECK_FAIL_CODE(lone.Value()->Call(p.Value(), "go"), ErrorCode::INVALID_ARGUMENT);
        }
    }
}

// ===========================================================================
// §13 Tick Safe Point：执行中热更暂存 / 限额中止回滚
// ===========================================================================

void TestTickSafePoint() {
    Probe probe;
    // 限额显式设置：默认 `max_instructions = 10M / max_exec_time = 5ms` 下，
    // `while true do end` 一定是**挂钟先到**（Timeout），断言 InstructionLimit 会失败。
    // 压小指令预算、放宽挂钟 → 「指令上限」确定性先触发，回滚路径才可稳定断言。
    LuaLimits limits;
    limits.max_instructions = 200000;
    limits.max_exec_time = mmo::core::DurationMs{5000};
    auto created = ScriptContext::Create(limits);
    CHECK(static_cast<bool>(created));
    if (!created) {
        return;
    }
    ScriptContext& ctx = *created.Value();
    AddProbeBinding(ctx, "probe.report", &ProbeReport, &probe, true);
    AddProbeBinding(ctx, "probe.hotload", &ProbeHotLoad, nullptr, false);

    CHECK(ctx.PendingCount() == 0u);
    CHECK(!ctx.InScriptExecution());
    CHECK_OK(ctx.ApplyPendingChanges());  // 空 pending 幂等

    // 执行中发起热更 → 暂存，不立即生效
    const auto driver = ctx.Load(
        "driver.lua",
        "function hot()\n"
        "  local id = probe.hotload('hot.lua', 'function h() probe.report(5) end')\n"
        "  probe.report(id)\n"
        "end");
    CHECK_OK(driver);
    probe.Reset();
    CHECK_OK(ctx.Call(driver.Value(), "hot"));
    CHECK(probe.count == 1u);
    CHECK(probe.values[0] > 0);          // 返回了目标 ScriptId
    CHECK(ctx.PendingCount() == 1u);     // 暂存未生效
    CHECK(ctx.LoadedCount() == 1u);      // 新脚本尚未登记（只有 driver.lua）
    CHECK(!ctx.IsLoaded(static_cast<mmo::script::ScriptId>(probe.values[0])));

    // 安全点统一应用
    CHECK_OK(ctx.ApplyPendingChanges());
    CHECK(ctx.PendingCount() == 0u);
    CHECK(ctx.LoadedCount() == 2u);
    const auto hot_id = static_cast<mmo::script::ScriptId>(probe.values[0]);
    CHECK(ctx.IsLoaded(hot_id));
    CHECK(ctx.NameOf(hot_id) == "hot.lua");
    CHECK_OK(ctx.Call(hot_id, "h"));
    CHECK(probe.Last() == 5);

    // 执行中热更 + 该次执行被限额中止 ⇒ 暂存变更**回滚丢弃**（§15.4）
    const auto aborter = ctx.Load(
        "aborter.lua",
        "function hot_and_die()\n"
        "  probe.hotload('doomed.lua', 'function d() end')\n"
        "  while true do end\n"
        "end");
    CHECK_OK(aborter);
    const std::size_t loaded_before = ctx.LoadedCount();
    const std::uint32_t version_before = ctx.Version();
    const auto aborted = ctx.Call(aborter.Value(), "hot_and_die");
    CHECK(!aborted);
    CHECK(ctx.LastError().code == ScriptError::InstructionLimit);
    CHECK(ctx.PendingCount() == 0u);            // 已回滚
    CHECK(ctx.LoadedCount() == loaded_before);  // doomed.lua 未被激活
    CHECK(ctx.Version() == version_before);

    // 卸载不存在的脚本 → NOT_FOUND
    CHECK_FAIL_CODE(ctx.Unload(9999), ErrorCode::NOT_FOUND);
}

// ===========================================================================
// §20.1 每 Scene 一个 VM，不跨线程共享
// ===========================================================================

void TestThreadAffinity() {
    auto created = ScriptContext::Create(LuaLimits{});
    CHECK(static_cast<bool>(created));
    if (!created) {
        return;
    }
    ScriptContext& ctx = *created.Value();

    const auto main_script = ctx.Load("main.lua", "function go() end");
    CHECK_OK(main_script);

    ErrorCode load_code = ErrorCode::OK;
    ErrorCode unload_code = ErrorCode::OK;
    ErrorCode call_code = ErrorCode::OK;
    ErrorCode apply_code = ErrorCode::OK;
    std::thread other([&ctx, id = main_script.Value(), &load_code, &unload_code, &call_code,
                       &apply_code]() {
        const auto l = ctx.Load("thread.lua", "function go() end");
        load_code = l ? ErrorCode::OK : l.Err().Code();
        const auto u = ctx.Unload(id);
        unload_code = u ? ErrorCode::OK : u.Err().Code();
        const auto c = ctx.Call(id, "go");
        call_code = c ? ErrorCode::OK : c.Err().Code();
        const auto a = ctx.ApplyPendingChanges();
        apply_code = a ? ErrorCode::OK : a.Err().Code();
    });
    other.join();
    CHECK(load_code == ErrorCode::BUSY);
    CHECK(unload_code == ErrorCode::BUSY);
    CHECK(call_code == ErrorCode::BUSY);
    CHECK(apply_code == ErrorCode::BUSY);

    // 本线程继续正常
    CHECK_OK(ctx.Load("main2.lua", "function go2() end"));
    CHECK(ctx.LoadedCount() == 2u);

    // 并发两个 VM（各自线程）各自独立可用
    std::atomic<int> successes{0};
    auto worker = [&successes]() {
        auto c = ScriptContext::Create(LuaLimits{});
        if (!c) {
            return;
        }
        ScriptContext& local_ctx = *c.Value();
        if (local_ctx.Load("w.lua", "function go() end")) {
            successes.fetch_add(1, std::memory_order_relaxed);
        }
        if (local_ctx.LoadedCount() == 1u) {
            successes.fetch_add(1, std::memory_order_relaxed);
        }
        if (local_ctx.Call(1u, "go")) {
            successes.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread t1(worker);
    std::thread t2(worker);
    t1.join();
    t2.join();
    CHECK(successes.load() == 6);
}

}  // namespace

int main() {
    ::mmo::core::test::Line("=== TASK-031 Lua Runtime unit/failure tests ===\n");
    TestSandboxAndBindingSurface();
    TestLoadUnloadAndErrorMapping();
    TestFourLimits();
    TestFiveBindings();
    TestEventBindings();
    TestTickSafePoint();
    TestThreadAffinity();

    if (failures == 0) {
        ::mmo::core::test::Line("ALL LUA RUNTIME TESTS PASSED\n");
        return 0;
    }
    ::mmo::core::test::ErrorFmt("%d TEST(S) FAILED\n", failures);
    return 1;
}
