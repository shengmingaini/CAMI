// scripting/lua/src/sandbox.cpp —— TASK-031 · 沙箱（§8 / §15.7 / §21）。
//
// 设计立场：**白名单，不是黑名单**（§21 硬约束）
// ------------------------------------------
//   朴素做法是「开全部库 → 把 io/os/debug/require 置 nil」。这是黑名单：Lua 一旦新增
//   任何能力（或某个库被惰性初始化），沙箱就静默失效。本实现改成三步重建：
//
//   1. 只开 **base / string / table / math** 四个库（io / os / package(require) / debug /
//      coroutine 从不打开）；
//   2. 用**显式成员白名单**重建 string / table / math 三张库表 —— 白名单外的成员
//      （如 `string.dump`）在新表里根本不存在；
//   3. 用**显式名字白名单**重建整张全局表（新的 `_G`）并写回 `LUA_RIDX_GLOBALS` ——
//      原全局表随之不可达，其上的 `load` / `loadfile` / `dofile` / `collectgarbage`
//      等一并消失。
//
//   于是「脚本能碰到什么」完全由下面的常量表决定，新增 Lua 能力不会自动进沙箱。
//
// Lua 5.5 专属捷径
// ---------------
//   Lua 5.5 新增 `luaL_openselectedlibs(L, load_mask, preload_mask)` —— 按位掩码开库，
//   天然就是白名单。5.4 及以前没有该函数，退回 `luaL_requiref` 逐个开。两条路径都只开
//   白名单内的库，语义一致。
//
// 实测逃逸点（已修，勿回退）
// ------------------------
//   **字符串类型的元表 `__index` 指向的是「原」string 表**。若只把白名单版 string 表设为
//   全局 `string`，脚本仍可用 `("x").dump` 拿到被排除的 `string.dump`。因此本文件在设置
//   全局 `string` 之前，显式把字符串元表的 `__index` 重定向到白名单版 string 表。

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "internal.h"

namespace mmo::script {
namespace {

// ---------------------------------------------------------------------------
// 白名单：脚本可见的全部名字
// ---------------------------------------------------------------------------

/// 全局表白名单（`_ENV.__index` 的终点）。
/// 刻意排除：load / loadfile / dofile（= loadstring 等价物与文件 IO）、collectgarbage
/// （限制 CPU 预算被脚本强行挪用）、warn（stderr IO）；io / os / package / debug /
/// coroutine 因从未开库而不存在。
/// 沙箱全局表白名单（`_G` 可见的名字）。
///
/// 【踩坑 · 必须含 `string` / `table` / `math` 三张库表】
/// 装配顺序是「先 `lua_setglobal` 装入**已按白名单重建**的 string/table/math，
/// 最后 `RebuildGlobals` 按本表从旧全局表里逐个搬到新全局表」。
/// 本表若漏掉这三张表，搬过去的新全局表里就没有它们 —— 脚本一写
/// `string.find(...)` / `math.floor(...)` 就报
/// `attempt to index a nil value (global 'string')`，且**沙箱装配本身是成功的**
/// （`OpenSandbox()` 返回 OK），错误只在脚本真正运行时才暴露，很隐蔽。
constexpr std::string_view kGlobalWhitelist[] = {
    "assert",  "error",  "getmetatable", "ipairs", "math",  "next",   "pairs",
    "pcall",   "print",  "rawequal",     "rawget", "rawlen", "rawset", "select",
    "setmetatable", "string", "table",   "tonumber", "tostring", "type", "xpcall",
    "_G",      "_VERSION",
};

/// `string` 白名单。排除 `string.dump`（字节码序列化 —— 与 load 组合可绕过源码审计）。
constexpr std::string_view kStringWhitelist[] = {
    "byte", "char",  "find", "format", "gmatch",   "gsub",  "len",     "lower",
    "match", "pack", "packsize", "rep", "reverse", "sub",   "unpack",  "upper",
};

/// `table` 白名单（全部成员均无 IO / 无越权能力）。
constexpr std::string_view kTableWhitelist[] = {
    "concat", "insert", "move", "pack", "remove", "sort", "unpack",
};

/// `math` 白名单。排除 `math.randomseed`：VM 创建时已用固定种子播种，
/// 允许脚本重新播种会破坏 Replay / 确定性测试（§25）。
constexpr std::string_view kMathWhitelist[] = {
    "abs",     "acos",  "asin",       "atan",      "ceil",       "cos",      "deg",
    "exp",     "floor", "fmod",       "huge",      "log",        "max",      "maxinteger",
    "min",     "mininteger", "modf",  "pi",        "rad",        "random",   "sin",
    "sqrt",    "tan",   "tointeger",  "type",      "ult",
};

template <std::size_t N>
constexpr std::size_t Count(const std::string_view (&)[N]) noexcept {
    return N;
}

// ---------------------------------------------------------------------------
// 白名单复制原语
// ---------------------------------------------------------------------------

/// 把 `source`（栈上索引）按白名单复制进一张新表，返回新表的绝对索引。
/// 白名单里不存在的成员被静默跳过（跨 Lua 版本名称差异不应导致装配失败）。
int BuildWhitelistedFrom(lua_State* state, int source, const std::string_view* names,
                        std::size_t count) {
    const int src = lua_absindex(state, source);
    lua_createtable(state, 0, static_cast<int>(count));
    const int fresh = lua_gettop(state);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string_view name = names[i];
        (void)detail::RawGetField(state, src, name);  // [..][value]
        if (lua_type(state, -1) == LUA_TNIL) {
            lua_pop(state, 1);
            continue;
        }
        detail::PushKey(state, name);                 // [..][value][key]
        lua_insert(state, -2);                        // [..][key][value]
        lua_rawset(state, fresh);                     // fresh[key] = value
    }
    return fresh;
}

/// 重建某个库的全局表；成功返回该新表在栈上的绝对索引（留在栈顶），失败返回 -1。
int RebuildLibrary(lua_State* state, const char* lib_name, const std::string_view* names,
                   std::size_t count) {
    (void)lua_getglobal(state, lib_name);  // [orig]
    if (lua_istable(state, -1) == 0) {
        lua_pop(state, 1);
        return -1;
    }
    const int orig = lua_gettop(state);
    // 返回值（新表索引）此处刻意不接：lua_remove 会使其过期，直接取 remove 后的栈顶更可靠。
    (void)BuildWhitelistedFrom(state, orig, names, count);  // [orig][fresh]
    lua_remove(state, orig);                                // [fresh]
    // 【踩坑】`lua_remove(orig)` 会把 `orig` 之上的元素整体下移一位，所以
    // `BuildWhitelistedFrom` 里取到的 `fresh` **已经过期**（比真实索引大 1）。
    // 直接 `return fresh` 会让调用方 `lua_pushvalue(state, fresh)` 压到隔壁那个值
    // —— 表现为「`mt.__index` 被设成了空字符串」而不是白名单 string 表，
    // 于是 `("x").dump` 索引字符串时又回到同一张元表 → 无限 __index 链 →
    // 运行期报 `'__index' chain too long; possible loop`。
    // remove 之后新表必定在栈顶，所以直接用 `lua_gettop` 取真实索引。
    return lua_gettop(state);
}

/// 用白名单重建整张全局表，写回 `LUA_RIDX_GLOBALS`（原表随之不可达）。
/// 这是「白名单而非黑名单」的落地点：白名单外的全局（load / dofile / …）不会出现在新表里。
bool RebuildGlobals(lua_State* state) {
    (void)lua_pushglobaltable(state);  // [orig]
    const int orig = lua_gettop(state);
    const std::size_t count = Count(kGlobalWhitelist);
    const int fresh = BuildWhitelistedFrom(state, orig, kGlobalWhitelist, count);  // [orig][fresh]

    // `_G` 自引用新表（脚本里 `_G.x = 1` 写的是沙箱全局，不是原表）。
    (void)lua_pushvalue(state, fresh);
    detail::RawSetField(state, fresh, "_G");

    // 关键：把新表写回 registry 的 globals 槽。`lua_getglobal` / `lua_setglobal` /
    // `lua_pushglobaltable` 都从该槽取值（Lua 5.4+ lapi.c），所以这一步真正换了全局环境。
    (void)lua_pushvalue(state, fresh);
    lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);

    lua_remove(state, fresh);  // [orig]
    lua_pop(state, 1);         // []
    return true;
}

/// 用固定种子播种 `math.random`（在 `math` 白名单重建**之前**做，
/// 之后 `randomseed` 不再对脚本可见，从而保持 Replay 确定性）。
void SeedDeterministicRandom(lua_State* state, std::uint64_t seed) {
    (void)lua_getglobal(state, LUA_MATHLIBNAME);
    if (lua_istable(state, -1) == 0) {
        lua_pop(state, 1);
        return;
    }
    (void)detail::RawGetField(state, -1, "randomseed");
    if (lua_isfunction(state, -1) == 0) {
        lua_pop(state, 2);
        return;
    }
    lua_remove(state, -2);  // [fn]
    lua_pushinteger(state, static_cast<lua_Integer>(seed));
    // 失败不影响安全（只是随机序列退化），故忽略返回值而不是中断装配。
    (void)lua_pcall(state, 1, 0, 0);
}

/// 打开白名单内的标准库。
void OpenWhitelistedLibs(lua_State* state) {
#if defined(LUA_VERSION_NUM) && (LUA_VERSION_NUM >= 505)
    // Lua 5.5：位掩码白名单开库（官方提供，天然白名单语义）。
    luaL_openselectedlibs(state, LUA_GLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_MATHLIBK, 0);
#else
    // Lua ≤5.4：逐个 requiref（仍未触碰 io / os / package / debug / coroutine）。
    luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state, 1);
#endif
}

/// 在**保护调用**内执行的沙箱装配体（返回 1 个布尔）。
/// 用 C 函数 + pcall 包裹的意义：装配期也需要分配内存，若 allocator 触顶则 Lua 会
/// 抛 LUA_ERRMEM —— 包在 pcall 里就变成可恢复错误，而不是走到 panic → abort（§19）。
int LuaSandboxSetup(lua_State* state) {
    OpenWhitelistedLibs(state);
    SeedDeterministicRandom(state, 0x2545F4914F6CDD1Du);

    // ---- string：白名单重建 + 修字符串元表 ----
    const int fresh_string =
        RebuildLibrary(state, LUA_STRLIBNAME, kStringWhitelist, Count(kStringWhitelist));
    if (fresh_string < 0) {
        lua_pushboolean(state, 0);
        return 1;
    }
    // 实测逃逸点：字符串元表的 __index 若仍指向原表，`("x").dump` 可绕过白名单。
    (void)lua_pushliteral(state, "");
    if (lua_getmetatable(state, -1) != 0) {
        (void)lua_pushvalue(state, fresh_string);
        lua_setfield(state, -2, "__index");  // mt.__index = 白名单版 string
        lua_pop(state, 1);
    }
    lua_pop(state, 1);  // pop 那个字符串
    lua_setglobal(state, LUA_STRLIBNAME);

    // ---- table / math：白名单重建 ----
    if (RebuildLibrary(state, LUA_TABLIBNAME, kTableWhitelist, Count(kTableWhitelist)) < 0) {
        lua_pushboolean(state, 0);
        return 1;
    }
    lua_setglobal(state, LUA_TABLIBNAME);

    if (RebuildLibrary(state, LUA_MATHLIBNAME, kMathWhitelist, Count(kMathWhitelist)) < 0) {
        lua_pushboolean(state, 0);
        return 1;
    }
    lua_setglobal(state, LUA_MATHLIBNAME);

    // ---- 全局表：白名单重建（**最后一步**，因为前面几步都要用原全局表）----
    (void)RebuildGlobals(state);

    lua_pushboolean(state, 1);
    return 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// LuaVM::OpenSandbox
// ---------------------------------------------------------------------------

core::Result<void> LuaVM::OpenSandbox() {
    lua_State* state = state_;
    lua_pushcfunction(state, &LuaSandboxSetup);
    const int rc = lua_pcall(state, 0, 1, 0);
    if (rc != LUA_OK) {
        // 装配失败（例如内存不足）：不把错误对象留在栈上（LuaVM 即将析构或复用）。
        lua_pop(state, 1);
        sandbox_open_ = false;
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "lua sandbox setup failed", core::domain::kLua));
    }
    const bool ok = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    if (!ok) {
        sandbox_open_ = false;
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "lua sandbox rebuilt lib missing", core::domain::kLua));
    }
    sandbox_open_ = true;
    return core::Result<void>::Ok();
}

}  // namespace mmo::script
