// scripting/lua/src/bind_command.cpp —— TASK-031 · 命令通道绑定（§8 Skill / Quest，§20.5 只走系统接口）。
//
// 为什么 Skill / Quest 不直连业务模块（关键设计取舍，§27.2 / §27.3）
// ----------------------------------------------------------------
// TASK-031 的依赖集是 TASK-001/004/005/007/011 —— **不含** combat(021) / quest(019)。
// 因此脚本侧的 `skill.cast` / `quest.*` 不得 `#include` 业务模块头。正解是把脚本请求表达成
// **普通 Command**（`ScriptCommand`，满足 core::CommandLike 的 5 个审计字段），经统一
// `core::CommandBus` 派发，由状态 Owner 注册的 op handler 执行：
//
//   Lua  skill.cast(caster, skill_id, target)
//     ->  ScriptCommand{op="skill.cast", args}
//     ->  core::CommandBus::Dispatch（调用者线程同步）
//     ->  宿主注册的 op handler（内部调 SkillSystem）
//     ->  ScriptReply -> Lua 表 {ok,i0,i1,d0}
//
// 三种收益：
//   1. 脚本改不了伤害数值 —— 它只能「请求施法」，数值由 SkillSystem 算（§8 禁止脚本直改伤害）；
//   2. 不是第二套消息格式（§4）：就是一条普通 Command，载荷为「op 名 + 参数表」；
//   3. 扩展靠 `ScriptContext::RegisterCommandOp` 注册（§27.4），新增 op **不需要改本文件**。
//
// 热路径（§10）：Dispatch 在同线程同步执行，`RegisterFn` 把载荷擦除为 `const void*` 不复制，
// 因此本文件不产生堆分配；`ScriptCommand` 是栈上定长结构（op 内联缓冲 + 定长 args）。

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "internal.h"

#include "mmo/core/bus/command.h"
#include "mmo/core/bus/command_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"
#include "mmo/script/script_event.h"

namespace mmo::script {
namespace {

/// 一次派发用的 `CommandContext`（只读入参，随调用链透传）。
/// source = kInternal 是**刻意**的：脚本不是「客户端」，其请求已在装配期由运营方审核源码。
core::CommandContext MakeCommandContext() {
    core::CommandContext ctx;
    ctx.source = core::CommandSource::kInternal;
    return ctx;
}

/// `FillScriptReply` 的签名适配：`SetResultTable` 的 fill 形参是 `void*`（非 const）。
void FillReplyTable(TableWriter& out, void* user) { detail::FillScriptReply(out, user); }

/// 参数个数校验（§19：脚本调用不存在的 API / 参数不对 → 明确错误而非崩溃）。
core::Result<void> RequireArgs(const ScriptCall& call, std::size_t expected) {
    if (call.ArgCount() != expected) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "wrong script arg count", core::domain::kLua));
    }
    return core::Result<void>::Ok();
}

/// `skill.cast(caster, skill_id, target)` —— §8 Skill。
core::Result<int> SkillCast(ScriptCall& call, void* /*user*/) {
    const core::Result<void> arity = RequireArgs(call, 3);
    if (!arity) {
        return core::Result<int>::Fail(arity.Err());
    }
    return detail::DispatchScriptCommand(call);
}

/// `quest.set_progress(player, idx, v)` —— §8 Quest。
core::Result<int> QuestSetProgress(ScriptCall& call, void* /*user*/) {
    const core::Result<void> arity = RequireArgs(call, 3);
    if (!arity) {
        return core::Result<int>::Fail(arity.Err());
    }
    return detail::DispatchScriptCommand(call);
}

/// `quest.complete(player, id)` —— §8 Quest。
core::Result<int> QuestComplete(ScriptCall& call, void* /*user*/) {
    const core::Result<void> arity = RequireArgs(call, 2);
    if (!arity) {
        return core::Result<int>::Fail(arity.Err());
    }
    return detail::DispatchScriptCommand(call);
}

}  // namespace

namespace detail {

bool PackArgs(const ScriptCall& call, std::size_t first, ScriptArgs& out) noexcept {
    for (std::size_t i = first; i < call.ArgCount(); ++i) {
        if (call.ArgIsTable(i)) {
            // 表参数按数组部分（1-based，与 Lua 一致）摊平；命名域字段不参与命令载荷
            // —— 命令载荷刻意保持「位置语义」，避免脚本用任意键名操纵业务语义。
            const ScriptTableView view = call.ArgTable(i);
            const std::size_t n = view.Size();
            for (std::size_t k = 1; k <= n; ++k) {
                if (!out.Push(view.GetIndex(k))) {
                    return false;
                }
            }
            continue;
        }
        if (!out.Push(call.Arg(i))) {
            return false;
        }
    }
    return true;
}

core::Result<int> DispatchScriptCommand(ScriptCall& call) {
    core::CommandBus* bus = call.Services().commands;
    if (bus == nullptr) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "command api not bound", core::domain::kLua));
    }
    // op 名 == 脚本可见名：注册表键与 Lua 路径同一字符串，杜绝「两套命名漂移」（§27.4）。
    const std::string_view op = call.Name();
    if (op.empty() || op.size() > kScriptOpMaxLen) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "script op name invalid", core::domain::kLua));
    }

    ScriptCommand cmd;
    if (!cmd.SetOp(op)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "script op name too long", core::domain::kLua));
    }
    if (!PackArgs(call, 0, cmd.args)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "too many script args", core::domain::kLua));
    }
    cmd.timestamp = core::MonotonicClock::Now();

    const core::Result<ScriptReply> reply = bus->Dispatch(cmd, MakeCommandContext());
    if (!reply) {
        // op 未注册 → NOT_FOUND（§19）；handler 抛异常 → INTERNAL_ERROR（由总线兜底）。
        return core::Result<int>::Fail(reply.Err());
    }
    ScriptReply value = reply.Value();
    call.SetResultTable(&FillReplyTable, static_cast<void*>(&value));
    return call.Done();
}

core::Result<int> DispatchScriptQuery(ScriptCall& call, std::string_view op) {
    core::QueryBus* bus = call.Services().queries;
    if (bus == nullptr) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "query api not bound", core::domain::kLua));
    }
    if (op.empty() || op.size() > kScriptOpMaxLen) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "script query op invalid", core::domain::kLua));
    }

    ScriptQuery query;
    if (!query.SetOp(op)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "script query op too long", core::domain::kLua));
    }
    if (!PackArgs(call, 1, query.args)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "too many script args", core::domain::kLua));
    }

    core::QueryContext ctx;
    ctx.request_id = core::kInvalidRequestId;
    const core::Result<ScriptReply> reply = bus->Ask(query, ctx);
    if (!reply) {
        return core::Result<int>::Fail(reply.Err());
    }
    ScriptReply value = reply.Value();
    call.SetResultTable(&FillReplyTable, static_cast<void*>(&value));
    return call.Done();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// 内置安装：§8 Skill / Quest 两个命名空间的绑定面
// ---------------------------------------------------------------------------

core::Result<void> ScriptContext::InstallCommandBindings() {
    const struct {
        const char* name;
        BindingKind kind;
        NativeFn fn;
        const char* description;
    } kBindings[] = {
        {"skill.cast", BindingKind::Skill, &SkillCast,
         "skill.cast(caster, skill_id, target) -> {ok,i0,i1,d0}；经 CommandBus 派发给 SkillSystem"},
        {"quest.set_progress", BindingKind::Quest, &QuestSetProgress,
         "quest.set_progress(player, idx, value) -> {ok,...}；经 CommandBus 派发给 QuestSystem"},
        {"quest.complete", BindingKind::Quest, &QuestComplete,
         "quest.complete(player, quest_id) -> {ok,...}；经 CommandBus 派发给 QuestSystem"},
    };

    for (const auto& binding : kBindings) {
        // read_only = false：这些是**命令**，语义上允许（且必须）产生副作用；
        // 但副作用由状态 Owner 执行，脚本自身仍然拿不到裸指针（§20.5 / §21）。
        const core::Result<void> added =
            InstallBindingEntry(binding.name, binding.kind, binding.fn, nullptr, false,
                                binding.description);
        if (!added) {
            return added;
        }
    }
    return core::Result<void>::Ok();
}

}  // namespace mmo::script
