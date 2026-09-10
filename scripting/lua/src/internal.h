#pragma once

/// TASK-031 · 模块内部头（**禁止被 include/ 下的公开头引用**，§27.3）。
///
/// 这里集中封装对 Lua C API 的直接使用，使 Lua 版本细节与 `-Wold-style-cast` /
/// `-Wconversion` 兼容性处理只出现在一处。

#include <cstddef>
#include <cstdint>
#include <string_view>

// Lua 头必须包在 extern "C" 里（**极易踩的坑，实测于 Lua 5.5.1 / MSYS2 MinGW64**）
// ------------------------------------------------------------------------------
// 这不是可选的风格问题，漏了会**编译通过但链接必失败**，且报错极具误导性：
//   ld: undefined reference to `lua_gettop(lua_State*)'   ← 注意带参数的 C++ 反解名
//   ld: undefined reference to `lua_pcallk(lua_State*, int, int, int, long long, ...)'
//
// 原因：上游 Lua 的 lua.h / lauxlib.h / lualib.h **刻意不带** `extern "C"` 守卫
// （Lua 自身也可能以 C++ 编译）。其官方 lua.hpp 的注释原文：
//   "extern "C" not supplied automatically in lua.h and other headers
//    because Lua also compiles as C++"
// 于是从 C++ 直接 `#include <lua.h>` 会按 **C++ 链接性**声明这些函数 → 符号被 mangle
// 成 `_Z10lua_gettopP9lua_State`，而 `liblua.dll.a` 导出的是 C 符号 `lua_gettop`，
// 两者对不上 → 全部 Lua API 未定义。
//
// 注意：`undefined reference to lua_xxx(lua_State*, int)` 里**带参数列表**就是
// "被 mangle 了"的判据；纯 C 符号 ld 只会打印 `lua_xxx` 不带括号。看到带参数就查 extern "C"。
//
// 修复方式二选一：(a) 本处显式包 extern "C"（采用，不依赖发行版是否装 lua.hpp）；
//                (b) 改用 `#include <lua.hpp>`（上游自带 extern "C" 包装）。
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "mmo/script/lua_vm.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_value.h"

namespace mmo::script::detail {

/// `void*`（公开头里的不透明句柄）↔ `lua_State*` 的唯一转换点。
inline lua_State* State(void* opaque) noexcept {
    return static_cast<lua_State*>(opaque);
}
inline void* Erase(lua_State* state) noexcept {
    return static_cast<void*>(state);
}

/// 把 `string_view` 作为表键压栈（避免为每次查找构造临时 `std::string`）。
inline void PushKey(lua_State* state, std::string_view key) {
    // key.data() 在 string_view 非空时有效；空 view 传 nullptr + 0 也合法。
    (void)lua_pushlstring(state, key.data(), key.size());
}

/// `table[key]`（rawget：不触发元方法 —— 绑定面的表是普通表）。
/// 返回压栈值的类型（LUA_TNIL 表示字段不存在）。
inline int RawGetField(lua_State* state, int table_index, std::string_view key) {
    const int table = lua_absindex(state, table_index);
    PushKey(state, key);
    return lua_rawget(state, table);
}

/// `table[key] = <栈顶值>`（rawset）。调用方须先把值压栈。
///
/// 【踩坑 · lua_rawset 的键值顺序】Lua 的 `lua_rawset(L, idx)` / `lua_settable` 取
/// **top-2 为键、top-1 为值**，即必须先压键、再压值：
///     lua_pushstring(L, "key"); lua_pushinteger(L, 42); lua_rawset(L, t);  -- t.key = 42
/// 本函数的契约是「调用方先把值压栈」，所以键是**后压**的，直接 rawset 会写反成
/// `t[value] = key` —— 表看起来「有内容」，但按名字取全是 nil，极其隐蔽
/// （实测症状：脚本拿到的 `r.ok` / `r.i0` 全为 nil；`_G` 也没有指向沙箱全局表，
/// 导致 `_G["io"]` 还能摸到未沙箱化的旧全局表）。
/// 因此这里用 `lua_insert(-2)` 把键换到值下面，凑成 rawset 期望的顺序。
/// （对照：`RawGetField` 只压键，`lua_rawget` 天然取栈顶为键，无需调整。）
inline void RawSetField(lua_State* state, int table_index, std::string_view key) {
    const int table = lua_absindex(state, table_index);
    PushKey(state, key);          // [.. value][key]
    lua_insert(state, -2);        // [.. key][value]
    lua_rawset(state, table);     // table[key] = value，弹出两者
}

/// 栈值 → `ScriptValue`。table / function / userdata 等复合类型一律映射为 `Nil`
/// （复合类型走 `ScriptTableView`，不塞进标量值 —— 避免绑定面语义含糊）。
ScriptValue ToScriptValue(lua_State* state, int index) noexcept;

/// 把 `ScriptValue` 压到 Lua 栈上（`Call` 传参、绑定返回值的统一通道）。
void PushScriptValue(lua_State* state, ScriptValue value);

/// 通用脚本事件的默认编解码：把 `ScriptEvent` 的标量参数按**数组部分**摊平进表。
/// `ev` 必须指向 `ScriptEvent`（否则视为空操作）。
void FillScriptEventArgs(TableWriter& out, const void* ev);

/// 把命令 / 查询的 `ScriptReply` 写成 Lua 表（脚本侧统一拿到 `{ok,i0,i1,d0}`）。
/// `reply` 必须指向 `ScriptReply`。
void FillScriptReply(TableWriter& out, const void* reply);

// ---------------------------------------------------------------------------
// 宿主错误码 ↔ Lua 错误文本（定义见 src/script_value.cpp）
// ---------------------------------------------------------------------------

/// `CopyErrorText`（声明见 mmo/script/script_binding.h）的**逆运算**：
/// 从 Lua 错误文本里把宿主原始 `core::ErrorCode` 还原出来。
///
/// 为什么需要它：错误跨「C++ 绑定 → lua_error/longjmp → lua_pcall → 调用方」这一圈之后，
/// Lua 只给出一个字符串和 `LUA_ERRRUN`。若不还原，脚本里一次「EntityId 失效」会退化成
/// 笼统的 `RuntimeError`（→ `INTERNAL_ERROR`），调用方再也分不出 NOT_FOUND /
/// INVALID_ARGUMENT / UNAUTHORIZED —— 而 §「失效 EntityId 返回 NOT_FOUND」正是要靠它成立。
///
/// 匹配 `/<CODE>: `（`<CODE>` 取 `core::ToString(ErrorCode)` 的非 OK 值），取**最早**一处。
/// 未匹配返回 false（调用方回落到 `ToCoreError(ScriptError)` 的映射）。
bool ParseCoreErrorCode(std::string_view message, core::ErrorCode* out) noexcept;

// ---------------------------------------------------------------------------
// 绑定实现的共享原语（定义见 src/bind_command.cpp）
// ---------------------------------------------------------------------------

/// 把脚本调用参数从第 `first` 个起打包成 `ScriptArgs`：
/// 标量原样入列；表参数按**数组部分**（1-based）摊平入列。
/// 超容量返回 false（**禁止静默截断**）。
bool PackArgs(const ScriptCall& call, std::size_t first, ScriptArgs& out) noexcept;

/// 把一次脚本命令经**真实** `core::CommandBus` 派发给状态 Owner，结果写成 Lua 表。
///
/// op 取 `call.Name()`（脚本可见名 == 注册表键，杜绝两套命名漂移，§27.4）。
/// 未绑定 CommandBus / op 未注册 / 派发失败 → 明确 core::Error（§19）。
core::Result<int> DispatchScriptCommand(ScriptCall& call);

/// 同上，只读查询版（经 `core::QueryBus`）；op 由调用方显式给出（`query.ask(name, …)`）。
core::Result<int> DispatchScriptQuery(ScriptCall& call, std::string_view op);

/// 从栈上第 `arg` 个参数取字符串；非字符串返回空视图。
std::string_view ToStringView(lua_State* state, int index) noexcept;

/// 统计某表数组部分的长度（`#table`）。
std::size_t TableArrayLen(lua_State* state, int index) noexcept;

/// 把 `std::size_t` 安全降到 `lua_Integer`（值域受 Lua 整数上限约束，超限返回 0）。
inline lua_Integer ToLuaIndex(std::size_t value) noexcept {
    return static_cast<lua_Integer>(value);
}

/// 统一的错误抛出点：把错误文本转成 Lua error（走 Lua 的错误机制，不是 C++ 异常）。
///
/// **UB 防护**：Lua 用 `longjmp` 实现错误抛出，会跳过 C++ 析构函数。
/// 因此调用本函数时**不得有任何存活的非平凡局部对象**。本模块所有绑定实现统一经
/// `MMO_SCRIPT_BIND_BODY` 宏出口，宿主自写绑定时也必须遵守同一约束。
[[noreturn]] void RaiseLuaError(lua_State* state, std::string_view message);

/// 解析 `"chunkname:line: text"` 中的脚本名与行号（实现见 src/lua_vm.cpp）。
/// 两个出参都可为 nullptr。返回是否解析出位置。
bool ParseScriptPosition(std::string_view message, std::string_view* out_script,
                         int* out_line) noexcept;

/// Lua 返回码 + 错误文本 → ScriptError（实现见 src/lua_vm.cpp）。
ScriptError MapLuaErrorCode(int lua_rc, std::string_view message) noexcept;

}  // namespace mmo::script::detail
