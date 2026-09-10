// scripting/lua/src/script_value.cpp —— TASK-031 · 值编解码与绑定注册表实现。
//
// 本文件是**唯一**把 Lua C API 暴露给公开值类型的地方（配合 internal.h）。
// 禁止 include 依赖模块的 src/（§27.2 / §27.3）。

#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "internal.h"

#include "mmo/core/error/error_code.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_event.h"
#include "mmo/script/script_value.h"

namespace mmo::script {
namespace detail {

ScriptValue ToScriptValue(lua_State* state, int index) noexcept {
    switch (lua_type(state, index)) {
        case LUA_TBOOLEAN:
            return ScriptValue::Bool(lua_toboolean(state, index) != 0);
        case LUA_TNUMBER:
            if (lua_isinteger(state, index) != 0) {
                return ScriptValue::Int(static_cast<std::int64_t>(lua_tointeger(state, index)));
            }
            return ScriptValue::Num(static_cast<double>(lua_tonumber(state, index)));
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(state, index, &len);
            return ScriptValue::Str(s != nullptr ? std::string_view(s, len) : std::string_view{});
        }
        default:
            // nil / table / function / userdata / thread → Nil（复合类型走 ScriptTableView）。
            return ScriptValue::Nil();
    }
}

std::string_view ToStringView(lua_State* state, int index) noexcept {
    if (lua_type(state, index) != LUA_TSTRING) {
        return {};
    }
    std::size_t len = 0;
    const char* s = lua_tolstring(state, index, &len);
    return s != nullptr ? std::string_view(s, len) : std::string_view{};
}

void PushScriptValue(lua_State* state, ScriptValue value) {
    switch (value.Type()) {
        case ScriptValueType::Nil:
            lua_pushnil(state);
            break;
        case ScriptValueType::Bool:
            lua_pushboolean(state, value.AsBool() ? 1 : 0);
            break;
        case ScriptValueType::Integer:
            lua_pushinteger(state, static_cast<lua_Integer>(value.AsInt()));
            break;
        case ScriptValueType::Number:
            lua_pushnumber(state, static_cast<lua_Number>(value.AsNum()));
            break;
        case ScriptValueType::String: {
            const std::string_view text = value.AsStr();
            (void)lua_pushlstring(state, text.data(), text.size());
            break;
        }
    }
}

std::size_t TableArrayLen(lua_State* state, int index) noexcept {
    const int abs = lua_absindex(state, index);
    if (lua_istable(state, abs) == 0) {
        return 0;
    }
    return static_cast<std::size_t>(lua_rawlen(state, abs));
}

void RaiseLuaError(lua_State* state, std::string_view message) {
    (void)lua_pushlstring(state, message.data(), message.size());
    (void)lua_error(state);  // longjmp：绝不返回
    // 不可达：仅为满足 [[noreturn]] 契约（Lua 5.5 的 lua_error 未标注 noreturn，
    // 缺这一句会触发 `-Wreturn-type` / "'noreturn' function does return"）。
    std::abort();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// 错误文本 → 平凡缓冲（longjmp 前的安全出口）
// ---------------------------------------------------------------------------

namespace detail {

std::size_t CopyErrorText(const core::Error& err, char* out, std::size_t cap) noexcept {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    std::size_t written = 0;
    const auto append = [&](std::string_view piece) {
        for (std::size_t i = 0; i < piece.size(); ++i) {
            if (written + 1 >= cap) {  // 预留结尾 '\0'
                return;
            }
            out[written] = piece[i];
            ++written;
        }
    };
    append(err.Domain());
    append("/");
    append(core::ToString(err.Code()));
    append(": ");
    append(err.Message());
    out[written] = '\0';
    return written;
}

bool ParseCoreErrorCode(std::string_view message, core::ErrorCode* out) noexcept {
    if (out == nullptr || message.empty()) {
        return false;
    }
    // 与 CopyErrorText 互逆：找**最早**出现的 `/<CODE>: `。
    // 逐 '/' 扫描而不是先构造 "/CODE: " 再 find，是为了：
    //   - 零分配（本函数在错误处理路径上，不能再依赖堆）；
    //   - 天然取最早匹配（脚本名若含 '/'，不会被误当成前缀）。
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (message[i] != '/') {
            continue;
        }
        const std::string_view tail = message.substr(i + 1);
        for (int raw = static_cast<int>(core::ErrorCode::OK) + 1;
             raw <= static_cast<int>(core::ErrorCode::INTERNAL_ERROR); ++raw) {
            const auto code = static_cast<core::ErrorCode>(raw);
            const std::string_view name = core::ToString(code);
            // 需要 "名字" + ": " 共 name.size() + 2 字节，故要求 size >= name.size() + 2。
            if (tail.size() > name.size() + 1 && tail.compare(0, name.size(), name) == 0 &&
                tail[name.size()] == ':' && tail[name.size() + 1] == ' ') {
                *out = code;
                return true;
            }
        }
    }
    return false;
}

void FillScriptReply(TableWriter& out, const void* reply) {
    const auto* r = static_cast<const ScriptReply*>(reply);
    if (r == nullptr) {
        return;
    }
    // 统一形状：脚本侧无需按 op 分支即可判定成败与取数（见 docs/INTERFACE.md §6）。
    out.SetBool("ok", r->valid);
    out.SetInt("i0", r->i0);
    out.SetInt("i1", r->i1);
    out.SetNum("d0", r->d0);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ScriptTableView
// ---------------------------------------------------------------------------

std::size_t ScriptTableView::Size() const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return 0;
    }
    return detail::TableArrayLen(state, index_);
}

bool ScriptTableView::Has(std::string_view key) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr || lua_istable(state, index_) == 0) {
        return false;
    }
    (void)detail::RawGetField(state, index_, key);
    const bool present = lua_type(state, -1) != LUA_TNIL;
    lua_pop(state, 1);
    return present;
}

ScriptValue ScriptTableView::Get(std::string_view key) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr || lua_istable(state, index_) == 0) {
        return ScriptValue::Nil();
    }
    (void)detail::RawGetField(state, index_, key);
    const ScriptValue value = detail::ToScriptValue(state, -1);
    lua_pop(state, 1);
    return value;
}

ScriptValue ScriptTableView::GetIndex(std::size_t i) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr || lua_istable(state, index_) == 0 || i == 0) {
        return ScriptValue::Nil();
    }
    (void)lua_rawgeti(state, index_, detail::ToLuaIndex(i));
    const ScriptValue value = detail::ToScriptValue(state, -1);
    lua_pop(state, 1);
    return value;
}

std::int64_t ScriptTableView::GetInt(std::string_view key, std::int64_t dflt) const noexcept {
    const ScriptValue v = Get(key);
    return v.IsNil() ? dflt : v.AsInt();
}

double ScriptTableView::GetNum(std::string_view key, double dflt) const noexcept {
    const ScriptValue v = Get(key);
    return v.IsNil() ? dflt : v.AsNum();
}

bool ScriptTableView::GetBool(std::string_view key, bool dflt) const noexcept {
    const ScriptValue v = Get(key);
    return v.IsNil() ? dflt : v.AsBool();
}

std::string_view ScriptTableView::GetStr(std::string_view key, std::string_view dflt) const noexcept {
    const ScriptValue v = Get(key);
    return v.IsNil() ? dflt : v.AsStr();
}

// ---------------------------------------------------------------------------
// TableWriter
// ---------------------------------------------------------------------------

void TableWriter::SetNil(std::string_view key) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return;
    }
    lua_pushnil(state);
    detail::RawSetField(state, index_, key);
}

void TableWriter::SetBool(std::string_view key, bool v) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return;
    }
    lua_pushboolean(state, v ? 1 : 0);
    detail::RawSetField(state, index_, key);
}

void TableWriter::SetInt(std::string_view key, std::int64_t v) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(v));
    detail::RawSetField(state, index_, key);
}

void TableWriter::SetNum(std::string_view key, double v) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return;
    }
    lua_pushnumber(state, static_cast<lua_Number>(v));
    detail::RawSetField(state, index_, key);
}

void TableWriter::SetStr(std::string_view key, std::string_view v) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return;
    }
    (void)lua_pushlstring(state, v.data(), v.size());
    detail::RawSetField(state, index_, key);
}

std::size_t TableWriter::PushInt(std::int64_t v) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return 0;
    }
    const auto len = static_cast<std::size_t>(lua_rawlen(state, index_)) + 1u;
    lua_pushinteger(state, static_cast<lua_Integer>(v));
    lua_rawseti(state, index_, detail::ToLuaIndex(len));
    return len;
}

std::size_t TableWriter::PushNum(double v) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return 0;
    }
    const auto len = static_cast<std::size_t>(lua_rawlen(state, index_)) + 1u;
    lua_pushnumber(state, static_cast<lua_Number>(v));
    lua_rawseti(state, index_, detail::ToLuaIndex(len));
    return len;
}

std::size_t TableWriter::PushBool(bool v) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return 0;
    }
    const auto len = static_cast<std::size_t>(lua_rawlen(state, index_)) + 1u;
    lua_pushboolean(state, v ? 1 : 0);
    lua_rawseti(state, index_, detail::ToLuaIndex(len));
    return len;
}

std::size_t TableWriter::PushStr(std::string_view v) const noexcept {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return 0;
    }
    const auto len = static_cast<std::size_t>(lua_rawlen(state, index_)) + 1u;
    (void)lua_pushlstring(state, v.data(), v.size());
    lua_rawseti(state, index_, detail::ToLuaIndex(len));
    return len;
}

// ---------------------------------------------------------------------------
// ScriptCall
// ---------------------------------------------------------------------------

ScriptValue ScriptCall::Arg(std::size_t i) const noexcept {
    if (i >= arg_count_ || state_ == nullptr) {
        return ScriptValue::Nil();
    }
    const int idx = arg_base_ + static_cast<int>(i);
    return detail::ToScriptValue(detail::State(state_), idx);
}

ScriptValue ScriptCall::ArgOr(std::size_t i, ScriptValue dflt) const noexcept {
    const ScriptValue v = Arg(i);
    return v.IsNil() ? dflt : v;
}

bool ScriptCall::ArgIsTable(std::size_t i) const noexcept {
    if (i >= arg_count_ || state_ == nullptr) {
        return false;
    }
    return lua_istable(detail::State(state_), arg_base_ + static_cast<int>(i)) != 0;
}

bool ScriptCall::ArgIsString(std::size_t i) const noexcept {
    if (i >= arg_count_ || state_ == nullptr) {
        return false;
    }
    return lua_type(detail::State(state_), arg_base_ + static_cast<int>(i)) == LUA_TSTRING;
}

bool ScriptCall::ArgIsInteger(std::size_t i) const noexcept {
    if (i >= arg_count_ || state_ == nullptr) {
        return false;
    }
    return lua_isinteger(detail::State(state_), arg_base_ + static_cast<int>(i)) != 0;
}

ScriptTableView ScriptCall::ArgTable(std::size_t i) const noexcept {
    if (!ArgIsTable(i)) {
        return ScriptTableView{};
    }
    return ScriptTableView(state_, arg_base_ + static_cast<int>(i));
}

int ScriptCall::ArgStackIndex(std::size_t i) const noexcept {
    if (i >= arg_count_ || state_ == nullptr) {
        return -1;
    }
    return arg_base_ + static_cast<int>(i);
}

core::Result<int> ScriptCall::Done() const {
    if (!has_result_) {
        // 未设置返回值 ⇒ 该次调用对脚本返回 0 个结果（Lua 侧为 nil）。
        return core::Result<int>::Ok(0);
    }
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return core::Result<int>::Ok(0);
    }
    if (result_table_index_ >= 0) {
        // 表由 SetResultTable 建好并已在栈上；约定：它是 Done() 之前的最后一项压栈操作。
        // 这里只校验它确实落在栈顶，避免调用方多压了东西导致返回值错位。
        if (lua_gettop(state) != result_table_index_) {
            lua_pushvalue(state, result_table_index_);
            return core::Result<int>::Ok(1);
        }
        return core::Result<int>::Ok(1);
    }
    detail::PushScriptValue(state, result_);
    return core::Result<int>::Ok(1);
}

void ScriptCall::SetResultTable(void (*fill)(TableWriter&, void*), void* user) {
    lua_State* state = detail::State(state_);
    if (state == nullptr) {
        return;
    }
    lua_createtable(state, 0, 8);
    const int idx = lua_gettop(state);
    if (fill != nullptr) {
        TableWriter writer(state_, idx);
        fill(writer, user);
    }
    result_table_index_ = idx;
    has_result_ = true;
}

void ScriptCall::RaiseError(std::string_view message) const {
    detail::RaiseLuaError(detail::State(state_), message);
}

// ---------------------------------------------------------------------------
// BindingKind 名称化
// ---------------------------------------------------------------------------

const char* ToString(BindingKind kind) noexcept {
    switch (kind) {
        case BindingKind::Entity:
            return "entity";
        case BindingKind::Skill:
            return "skill";
        case BindingKind::Quest:
            return "quest";
        case BindingKind::Event:
            return "event";
        case BindingKind::Query:
            return "query";
        case BindingKind::Native:
            return "native";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// BindingRegistry
// ---------------------------------------------------------------------------

core::Result<void> BindingRegistry::Add(BindingDef def) {
    if (def.name.empty()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "empty binding name", core::domain::kLua));
    }
    if (def.fn == nullptr) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "null binding fn", core::domain::kLua));
    }
    if (frozen_) {
        // 冻结后禁止改绑定表：运行期改绑定会让脚本行为漂移（§19 防静默覆盖的同源要求）。
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "binding registry frozen", core::domain::kLua));
    }
    if (index_.find(std::string_view(def.name)) != index_.end()) {
        // 禁止静默覆盖（与 TASK-007 CommandBus / QueryBus 口径一致）。
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "duplicate binding name", core::domain::kLua));
    }
    const std::size_t idx = defs_.size();
    const std::string key = def.name;
    defs_.push_back(std::move(def));
    index_.emplace(key, idx);
    return core::Result<void>::Ok();
}

core::Result<const BindingDef*> BindingRegistry::Find(std::string_view name) const noexcept {
    const auto it = index_.find(name);
    if (it == index_.end()) {
        return core::Result<const BindingDef*>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "binding not registered", core::domain::kLua));
    }
    return core::Result<const BindingDef*>::Ok(&defs_[it->second]);
}

bool BindingRegistry::Contains(std::string_view name) const noexcept {
    return index_.find(name) != index_.end();
}

std::size_t BindingRegistry::SizeOf(BindingKind kind) const noexcept {
    std::size_t n = 0;
    for (const BindingDef& def : defs_) {
        if (def.kind == kind) {
            ++n;
        }
    }
    return n;
}

}  // namespace mmo::script
