// scripting/lua/src/bind_query.cpp —— TASK-031 · 只读查询绑定（§8 Query，§21 运行期禁止副作用）。
//
// 契约（§8 绑定面）
// ----------------
//   query.ask(name, table) -> {ok,i0,i1,d0}
//
//   - 第 1 个参数是 **op 名**（字符串），第 2 个起是查询参数：表参数按数组部分摊平，
//     标量原样入列（与命令通道同一套打包规则，见 detail::PackArgs）。
//   - 必须经**真实** `core::QueryBus::Ask` 执行：Ask 期间会置起只读区间（thread_local），
//     测试替身 SideEffectProbe 可据此捕获任何违规写入（§17 集成测试要求）。
//   - `read_only = true`：本绑定被登记为只读断言；绑定面**没有**任何能让脚本写状态的 query 入口。
//
// 为什么查询也要走总线而不是直连 EntityManager
// ------------------------------------------
//   直连会绕过只读区间，等于「脚本可以顺手改点东西而无人察觉」。走 Ask 则把
//   「只读」做成**可被测试断言**的属性，而不是口头约定（§30 Correctness 优先）。

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "internal.h"

#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"

namespace mmo::script {
namespace {

/// `query.ask(name, ...)` —— §8 Query。
core::Result<int> QueryAsk(ScriptCall& call, void* /*user*/) {
    if (call.ArgCount() < 1) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "query.ask needs op name", core::domain::kLua));
    }
    if (!call.ArgIsString(0)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "query op must be string", core::domain::kLua));
    }
    const std::string_view op = call.Arg(0).AsStr();
    if (op.empty()) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "query op must be string", core::domain::kLua));
    }
    return detail::DispatchScriptQuery(call, op);
}

}  // namespace

core::Result<void> ScriptContext::InstallQueryBindings() {
    return InstallBindingEntry(
        "query.ask", BindingKind::Query, &QueryAsk, nullptr, /*read_only=*/true,
        "query.ask(op, ...) -> {ok,i0,i1,d0}；经 QueryBus 只读执行，运行期禁止副作用");
}

}  // namespace mmo::script
