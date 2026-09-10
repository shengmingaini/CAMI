// scripting/lua/src/script_context.cpp —— TASK-031 · ScriptContext（§7 / §13 / §15.4 / §19 / §20.1）。
//
// 本文件承载三类职责：
//   1. 脚本生命周期：Load（编译 → 校验 → 激活）、Unload、Call；
//   2. §13 Tick Safe Point：脚本执行中的热变更暂存，安全点统一切换，中止则回滚；
//   3. 错误映射：Lua 返回码 + 错误消息 → ScriptError → core::Error（**含脚本名与行号**）。
//
// 线程纪律：所有入口先查 `vm_->OnOwnerThread()`，跨线程一律 BUSY ——
// 这是 §20.1「每 Scene 一个 VM，不跨线程共享」的**强制**实现（不是口头约定）。
//
// longjmp 与 C++ 析构（**容易踩的 UB 陷阱**）
// -----------------------------------------
// Lua 用 longjmp 抛错。**受保护调用**（lua_pcall）的 longjmp 落在 Lua 自己的 setjmp 里，
// 也就是落在我们 `lua_pcall` 这层 C++ 帧**之下**，因此本文件的 RAII（ExecScope 等）
// 会正常析构，安全。
// 但**主动 raise**（lua_error）会跳过从 raise 点到最近受保护边界的全部 C++ 帧 ——
// 所以凡是「先构造 core::Result/std::string 再 raise」的地方，都必须先用内层作用域把
// 非平凡对象析构干净，再用**平凡缓冲**（char[]）raise。见 BindingEntry 与
// detail::CopyErrorText。

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "internal.h"

#include "mmo/script/script_context.h"

namespace mmo::script {
namespace {

/// 本模块在 Lua registry 里存放 `ScriptContext*` 的键。
/// 用「进程内唯一地址」当 lightuserdata 键是标准做法：不占用字符串名字空间，
/// 也不会与脚本可见的名字冲突。
char kContextRegistryKey = 0;

/// 取回绑定函数所属的 ScriptContext。
ScriptContext* ContextOf(lua_State* state) {
    lua_pushlightuserdata(state, static_cast<void*>(&kContextRegistryKey));
    (void)lua_rawget(state, LUA_REGISTRYINDEX);
    auto* ctx = static_cast<ScriptContext*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return ctx;
}

/// 在 Lua registry 中登记 `ScriptContext*`（每 VM 一次）。
void BindContextToState(lua_State* state, ScriptContext* ctx) {
    lua_pushlightuserdata(state, static_cast<void*>(&kContextRegistryKey));
    lua_pushlightuserdata(state, static_cast<void*>(ctx));
    lua_rawset(state, LUA_REGISTRYINDEX);
}

/// pcall 的 message handler：保证错误文本带 `"chunkname:line: "` 前缀（§15.9）。
int LuaErrorHandler(lua_State* state) {
    if (lua_type(state, 1) != LUA_TSTRING) {
        (void)luaL_tolstring(state, 1, nullptr);
        return 1;
    }
    std::size_t len = 0;
    const char* text = lua_tolstring(state, 1, &len);
    if (text == nullptr) {
        lua_pushliteral(state, "unknown lua error");
        return 1;
    }
    if (detail::ParseScriptPosition(std::string_view(text, len), nullptr, nullptr)) {
        return 1;  // 已有位置信息，原样透传
    }
    luaL_where(state, 1);
    (void)lua_pushlstring(state, text, len);
    lua_concat(state, 2);
    return 1;
}

/// 把栈顶错误对象转成 `LuaVM::ErrorDetail` 并写入 VM。
///
/// `forced != Ok` 表示这是 debug hook 主动中止的结果 —— 此时错误串是我们自己写的
/// （`"lua: instruction limit exceeded"`），Lua 返回码可能仍是 LUA_ERRRUN，
/// 所以必须优先采用 hook 记录的原因，否则会退化成「RuntimeError」丢失真实语义。
void CaptureLuaError(LuaVM& vm, int lua_rc, ScriptError forced, lua_State* state) {
    LuaVM::ErrorDetail detail;
    std::string_view message;
    if (lua_type(state, -1) == LUA_TSTRING) {
        std::size_t len = 0;
        const char* text = lua_tolstring(state, -1, &len);
        if (text != nullptr) {
            message = std::string_view(text, len);
        }
    }
    detail.text.assign(message);
    detail.code = forced != ScriptError::Ok ? forced
                                            : detail::MapLuaErrorCode(lua_rc, message);
    std::string_view script;
    int line = 0;
    if (detail::ParseScriptPosition(message, &script, &line)) {
        detail.script.assign(script);
        detail.line = line;
    }
    vm.SetLastError(std::move(detail));
}

/// 进入/退出脚本执行区间：预算（ExecScope）+ 执行深度 + pending 回滚标记。
///
/// 中止回滚（§15.4「回滚到安全点」）：若本次执行因限额被中止，则把 `pending_` 截断回
/// 进入时的长度 —— 即「脚本执行中途发起的 Load/Unload 全部作废」。
class RunGuard {
public:
    RunGuard(ScriptContext& ctx, LuaVM& vm)
        : ctx_(ctx), vm_(vm), scope_(vm), depth_mark_(ctx.RunDepth()),
          pending_mark_(ctx.PendingMark()),
          alloc_fails_before_(vm.AllocFailCalls()) {
        ctx_.EnterRun(depth_mark_);
    }

    ~RunGuard() {
        ctx_.ExitRun(depth_mark_, scope_.Aborted() ? pending_mark_ : kNoRollback);
    }

    bool Aborted() const noexcept { return scope_.Aborted(); }
    ScriptError AbortReason() const noexcept { return scope_.AbortReason(); }

    /// 本次执行期间是否发生过**分配触顶**。
    ///
    /// 为什么需要它：内存耗尽的真实返回码经常不是 LUA_ERRMEM。Lua 在 OOM 时用预分配的
    /// 错误串抛出，然后在 **message handler** 里做 `luaL_where` / `lua_concat`
    /// ——这两步还要分配内存，于是二次失败把返回码变成 LUA_ERRERR，
    /// 真实原因（内存）被掩盖成 RuntimeError。用「本区间内分配失败次数增加」
    /// 作为独立判据，可以把内存超限稳定还原出来（§16/§19 要求内存限制可被断言）。
    bool AllocationFailed() const noexcept { return vm_.AllocFailCalls() > alloc_fails_before_; }

private:
    static constexpr std::size_t kNoRollback = static_cast<std::size_t>(-1);
    ScriptContext& ctx_;
    LuaVM& vm_;
    LuaVM::ExecScope scope_;
    std::uint32_t depth_mark_;
    std::size_t pending_mark_;
    std::size_t alloc_fails_before_{0};
};

/// 归并「限额中止」与「分配触顶」为强制错误原因（Ok = 交给返回码映射）。
ScriptError ForcedReason(bool aborted, ScriptError reason, bool alloc_failed) noexcept {
    if (aborted) {
        return reason;
    }
    return alloc_failed ? ScriptError::MemoryLimit : ScriptError::Ok;
}

// RunGuard 需要访问 ScriptContext 的私有深度/队列 —— 用最小友元面暴露三个操作。
// （放在匿名 namespace 里无法声明友元，故这三个操作作为 ScriptContext 的公开内部方法，
//   命名带下划线前缀并标注「内部使用」。）

}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构 / 工厂
// ---------------------------------------------------------------------------

ScriptContext::ScriptContext(std::unique_ptr<LuaVM> vm) : vm_(std::move(vm)) {}

ScriptContext::~ScriptContext() {
    // 退订全部 C++ 侧订阅（**必须**）：订阅 lambda 捕获了本对象指针，若总线比本对象长寿
    // 而订阅未撤，后续 Drain 会回调已析构的对象 → 悬垂调用。
    if (services_.events != nullptr) {
        for (const core::EventBus::SubId id : bridge_subs_) {
            (void)services_.events->Unsubscribe(id);  // 幂等：已退订/非法 ID 同样返回 OK
        }
    }
    bridge_subs_.clear();

    // 释放脚本事件回调的 Lua registry 引用。注意：这一步必须在 vm_ 成员析构**之前**做，
    // 而析构函数体恰好在成员析构之前执行，故是安全的。
    if (vm_ != nullptr) {
        lua_State* state = vm_->NativeState();
        if (state != nullptr) {
            for (const Subscriber& sub : subscribers_) {
                if (sub.fn_ref >= 0) {
                    luaL_unref(state, LUA_REGISTRYINDEX, sub.fn_ref);
                }
            }
        }
    }
}

core::Result<std::unique_ptr<ScriptContext>> ScriptContext::Create(LuaLimits limits) {
    auto vm_result = LuaVM::Create(limits);
    if (!vm_result) {
        return core::Result<std::unique_ptr<ScriptContext>>::Fail(vm_result.Err());
    }
    std::unique_ptr<ScriptContext> ctx(
        new ScriptContext(std::move(vm_result).Value()));

    // 让所有绑定入口能从 lua_State 反查 context。
    BindContextToState(ctx->vm_->NativeState(), ctx.get());

    // 装配内置绑定面（五类）。这四步只注册 Lua 侧入口，不要求上游系统已绑定 ——
    // services_ 为空时由各绑定实现返回 INVALID_ARGUMENT（§19：
    // 「脚本调用不存在的 API 返回明确错误而非崩溃」）。
    const core::Result<void> installs[] = {
        ctx->InstallEntityBindings(),
        ctx->InstallCommandBindings(),
        ctx->InstallQueryBindings(),
        ctx->InstallEventBindings(),
    };
    for (const core::Result<void>& step : installs) {
        if (!step) {
            return core::Result<std::unique_ptr<ScriptContext>>::Fail(step.Err());
        }
    }
    return core::Result<std::unique_ptr<ScriptContext>>::Ok(std::move(ctx));
}

// ---------------------------------------------------------------------------
// 内部方法（供 RunGuard 使用；命名带下划线前缀表示「非稳定 API」）
// ---------------------------------------------------------------------------

std::uint32_t ScriptContext::RunDepth() const noexcept {
    return run_depth_;
}

std::size_t ScriptContext::PendingMark() const noexcept {
    return pending_.size();
}

void ScriptContext::EnterRun(std::uint32_t /*mark*/) noexcept {
    ++run_depth_;
}

void ScriptContext::ExitRun(std::uint32_t /*depth_mark*/, std::size_t rollback_to) noexcept {
    if (run_depth_ > 0) {
        --run_depth_;
    }
    if (rollback_to != static_cast<std::size_t>(-1) && rollback_to <= pending_.size()) {
        // 中止回滚：本次执行期间暂存的脚本变更全部作废。
        for (std::size_t i = rollback_to; i < pending_.size(); ++i) {
            const PendingOp& op = pending_[i];
            if (op.kind == PendingOp::Kind::Load && op.chunk_ref >= 0 && vm_ != nullptr) {
                luaL_unref(vm_->NativeState(), LUA_REGISTRYINDEX, op.chunk_ref);
            }
        }
        pending_.resize(rollback_to);
    }
}

// ---------------------------------------------------------------------------
// §7 Bind*Api
// ---------------------------------------------------------------------------

core::Result<void> ScriptContext::BindEntityApi(game::EntityManager& entities) {
    services_.entities = &entities;
    return core::Result<void>::Ok();
}

core::Result<void> ScriptContext::BindEventApi(core::EventBus& events) {
    services_.events = &events;

    // 通用脚本事件只订阅一次：脚本用 event.publish 发的**自定义名字**事件都经
    // `ScriptEvent` 走真实 EventBus（异步），此处安装唯一的桥接订阅。
    // 订阅 ID 记录在 bridge_subs_，析构时退订（lambda 捕获了 this）。
    auto sub = events.Subscribe<ScriptEvent>([this](const ScriptEvent& ev) {
        // 无订阅者 / 单条回调失败都不应打断 Drain（§19 派发隔离），故忽略返回值。
        (void)DispatchEventToSubscribers(ev.Name(), &detail::FillScriptEventArgs,
                                         static_cast<const void*>(&ev));
    });
    if (!sub) {
        return core::Result<void>::Fail(sub.Err());
    }
    bridge_subs_.push_back(sub.Value());
    return core::Result<void>::Ok();
}

core::Result<void> ScriptContext::BindCommandApi(core::CommandBus& commands) {
    services_.commands = &commands;
    // 脚本命令统一经此 handler 路由到 op 注册表。由本模块注册**一次**，
    // 业务系统只需用 RegisterCommandOp 声明自己能处理哪些 op（§27.4 注册表机制）。
    // 重复调用 BindCommandApi 会因「禁止静默覆盖」而返回 INVALID_ARGUMENT（TASK-007 口径）。
    const auto registered = commands.RegisterFn<ScriptCommand>(
        [this](const ScriptCommand& cmd,
               const core::CommandContext& /*ctx*/) -> core::Result<ScriptReply> {
            return RouteCommandOp(cmd);
        });
    if (!registered) {
        return core::Result<void>::Fail(registered.Err());
    }
    return core::Result<void>::Ok();
}

core::Result<void> ScriptContext::RegisterCommandOp(std::string_view op, ScriptCommandHandler handler,
                                                   void* user) {
    if (op.empty() || handler == nullptr) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "invalid command op", core::domain::kLua));
    }
    if (command_ops_.find(op) != command_ops_.end()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "duplicate command op", core::domain::kLua));
    }
    const std::size_t index = command_handlers_.size();
    command_handlers_.emplace_back(handler, user);
    command_ops_.emplace(std::string(op), index);
    return core::Result<void>::Ok();
}

core::Result<void> ScriptContext::RegisterQueryOp(std::string_view op, ScriptQueryHandler handler,
                                                 void* user) {
    if (op.empty() || handler == nullptr) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "invalid query op", core::domain::kLua));
    }
    if (query_ops_.find(op) != query_ops_.end()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "duplicate query op", core::domain::kLua));
    }
    const std::size_t index = query_handlers_.size();
    query_handlers_.emplace_back(handler, user);
    query_ops_.emplace(std::string(op), index);
    return core::Result<void>::Ok();
}

core::Result<ScriptReply> ScriptContext::RouteCommandOp(const ScriptCommand& cmd) const {
    const auto it = command_ops_.find(cmd.Op());
    if (it == command_ops_.end()) {
        // §19：脚本调用未注册的能力 → 明确错误（NOT_FOUND），不是崩溃、也不是静默 nil。
        return core::Result<ScriptReply>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "script command op not registered", core::domain::kLua));
    }
    const auto& entry = command_handlers_[it->second];
    return entry.first(cmd, entry.second);
}

core::Result<ScriptReply> ScriptContext::RouteQueryOp(const ScriptQuery& query) const {
    const auto it = query_ops_.find(query.Op());
    if (it == query_ops_.end()) {
        return core::Result<ScriptReply>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "script query op not registered", core::domain::kLua));
    }
    const auto& entry = query_handlers_[it->second];
    return entry.first(query, entry.second);
}

core::Result<void> ScriptContext::BindQueryApi(core::QueryBus& queries) {
    services_.queries = &queries;
    // 只读路线：脚本查询经 QueryBus 的只读区间执行（§8 / §21 禁止 Query 有副作用）。
    const auto registered = queries.RegisterFn<ScriptQuery>(
        [this](const ScriptQuery& query,
               const core::QueryContext& /*ctx*/) -> core::Result<ScriptReply> {
            return RouteQueryOp(query);
        });
    if (!registered) {
        return core::Result<void>::Fail(registered.Err());
    }
    return core::Result<void>::Ok();
}

std::size_t ScriptContext::MemoryUsed() const noexcept {
    return vm_->MemoryUsed();
}

std::uint32_t ScriptContext::Version() const noexcept {
    return vm_->Version();
}

bool ScriptContext::IsLoaded(ScriptId id) const noexcept {
    return id != kInvalidScriptId && id <= scripts_.size() &&
           scripts_[id - 1].module_ref >= 0;
}

std::size_t ScriptContext::LoadedCount() const noexcept {
    // 与 IsLoaded 同口径：只数仍然持有模块表引用的槽位。
    // 卸载保留槽位（防 id 复用），所以不能用 scripts_.size()。
    std::size_t live = 0;
    for (const ScriptEntry& entry : scripts_) {
        if (entry.module_ref >= 0) {
            ++live;
        }
    }
    return live;
}

std::string_view ScriptContext::NameOf(ScriptId id) const noexcept {
    if (!IsLoaded(id)) {
        return {};
    }
    return scripts_[id - 1].name;
}

core::Error ScriptContext::MakeError(ScriptError code) const noexcept {
    const LuaVM::ErrorDetail& detail = vm_->LastError();
    std::string msg;
    msg.reserve(detail.script.size() + detail.text.size() + 32);
    msg.append(ToString(code));
    if (!detail.script.empty()) {
        msg.push_back(' ');
        msg.append(detail.script);
        msg.push_back(':');
        msg.append(std::to_string(detail.line));
    }
    if (!detail.text.empty()) {
        msg.append(": ");
        msg.append(detail.text);
    }
    // 错误码优先级：宿主原始码 > ScriptError 的静态映射。
    //
    // 绑定函数失败时会在 Lua 错误串里留下 `<domain>/<CODE>: msg`（见 CopyErrorText），
    // 经 longjmp → lua_pcall 之后只剩这个字符串，所以这里把它还原回来。
    // 少了这一步，"EntityId 失效 → NOT_FOUND" 会退化成笼统的 INTERNAL_ERROR。
    core::ErrorCode core_code = ToCoreError(code).Code();
    core::ErrorCode recovered = core::ErrorCode::OK;
    if (detail::ParseCoreErrorCode(detail.text, &recovered)) {
        core_code = recovered;
    }
    return core::Error(core_code, msg, core::domain::kLua);
}

// ---------------------------------------------------------------------------
// 编译 / 执行 chunk
// ---------------------------------------------------------------------------

core::Result<void> ScriptContext::CompileChunk(std::string_view name, std::string_view source,
                                               int* out_chunk_ref) {
    lua_State* state = vm_->NativeState();
    const int base = lua_gettop(state);

    // chunkname 以 '@' 开头 ⇒ Lua 在错误消息里按「文件名」显示（即 name:line: 形式）。
    std::string chunk_name;
    chunk_name.reserve(name.size() + 1);
    chunk_name.push_back('@');
    chunk_name.append(name);

    // mode "t"：**只接受文本 chunk，拒绝预编译字节码** —— 这是对抗「绕过源码审计
    // 投放字节码」的第二道闸（第一道是白名单里没有 string.dump）。
    const int rc = luaL_loadbufferx(state, source.data(), source.size(), chunk_name.c_str(), "t");
    if (rc != LUA_OK) {
        CaptureLuaError(*vm_, rc, ScriptError::Ok, state);
        lua_settop(state, base);
        return core::Result<void>::Fail(MakeError(ScriptError::CompileError));
    }
    const int ref = luaL_ref(state, LUA_REGISTRYINDEX);  // 弹出 chunk
    lua_settop(state, base);
    *out_chunk_ref = ref;
    return core::Result<void>::Ok();
}

core::Result<void> ScriptContext::RunCompiledChunk(std::string_view name, int chunk_ref,
                                                  int env_ref, int* out_module_ref,
                                                  int* out_env_ref) {
    lua_State* state = vm_->NativeState();
    const int base = lua_gettop(state);
    *out_env_ref = -1;

    // ---- `_ENV`：新建 or 复用 ----
    //
    // 复用路径（`env_ref >= 0`，原地热替换，TASK-032）**不是**直接把旧环境当新环境用，
    // 而是「新建环境 + 浅拷贝旧环境里的**非函数**值」。原因：
    //   1. 直接复用同一张表 ⇒ 新版本删掉的旧函数仍留在表里（删不掉，因为不知道新版本会定义
    //      哪些名字）→ 线上处于「半新半旧」，违反 TASK-032 §21；
    //   2. 让新环境 `__index` 指向旧环境（链式继承）⇒ 连续热更会拉出 v1→v2→…→vn 的
    //      `__index` 链，内存不释放（违反 §19「连续 10 次热更不泄漏」），且链过长会触发
    //      Lua 的 `'__index' chain too long; possible loop`（TASK-031 已实测踩过）。
    // 浅拷贝非函数值则同时满足三点：状态连续保留、旧函数被干净替换、内存有界。
    // （被排除的「函数型全局」若是脚本刻意持久化的闭包，会在热更时丢失 —— 已写入
    //   docs/HOTRELOAD.md §4 作为显式契约。）
    int env = -1;
    if (env_ref >= 0) {
        (void)lua_rawgeti(state, LUA_REGISTRYINDEX, env_ref);  // [old_env]
        if (lua_istable(state, -1) == 0) {
            lua_settop(state, base);
            return core::Result<void>::Fail(core::Error(
                core::ErrorCode::INTERNAL_ERROR, "env ref not a table", core::domain::kLua));
        }
        const int old_env = lua_gettop(state);

        (void)lua_pushglobaltable(state);
        const int globals = lua_gettop(state);
        lua_createtable(state, 0, 2);
        env = lua_gettop(state);
        lua_createtable(state, 0, 1);
        (void)lua_pushvalue(state, globals);
        lua_setfield(state, -2, "__index");
        lua_setmetatable(state, env);

        // 浅拷贝旧环境：跳过函数值（函数由新版本源码重新定义）。
        (void)lua_pushnil(state);  // [old_env][globals][env][nil]
        while (lua_next(state, old_env) != 0) {
            // 栈：… [key][value]
            if (lua_isfunction(state, -1) == 0) {
                (void)lua_pushvalue(state, -2);  // [key][value][key]
                lua_insert(state, -2);           // [key][key][value]
                lua_rawset(state, env);          // env[key] = value（弹出 key/value）
            } else {
                lua_pop(state, 1);  // 丢函数值，保留 key 供 lua_next 继续
            }
        }

        lua_settop(state, env);  // 丢掉 globals 与 old_env：env 的 __index 已持 _G 引用
    } else {
        // ---- 新建独立 _ENV：`__index = _G`，写落在本脚本私有表，读回落到沙箱全局 ----
        // 收益：脚本 A 的全局写不会污染脚本 B（§4 单 Owner 精神），卸载即整体回收。
        (void)lua_pushglobaltable(state);
        const int globals = lua_gettop(state);
        lua_createtable(state, 0, 2);
        env = lua_gettop(state);
        lua_createtable(state, 0, 1);
        (void)lua_pushvalue(state, globals);
        lua_setfield(state, -2, "__index");
        lua_setmetatable(state, env);
        lua_settop(state, env);  // 丢掉 globals：env 的 __index 已持有引用
    }

    lua_pushcfunction(state, &LuaErrorHandler);
    const int msgh = lua_gettop(state);

    (void)lua_rawgeti(state, LUA_REGISTRYINDEX, chunk_ref);  // [env][msgh][chunk]
    if (lua_isfunction(state, -1) == 0) {
        lua_settop(state, base);
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INTERNAL_ERROR,
                                                    "chunk ref not a function",
                                                    core::domain::kLua));
    }
    const int chunk = lua_gettop(state);

    (void)lua_pushvalue(state, env);
    (void)lua_setupvalue(state, chunk, 1);  // chunk._ENV = env（弹出）→ 栈顶回到 chunk

    // ---- 执行（受控：指令 / 时间预算 + 栈深 + pending 回滚）----
    int rc = LUA_OK;
    ScriptError forced = ScriptError::Ok;
    {
        RunGuard guard(*this, *vm_);
        rc = lua_pcall(state, 0, 1, msgh);
        forced = ForcedReason(guard.Aborted(), guard.AbortReason(), guard.AllocationFailed());
    }

    if (rc != LUA_OK) {
        CaptureLuaError(*vm_, rc, forced, state);
        lua_settop(state, base);
        const ScriptError code =
            forced != ScriptError::Ok ? forced
                                      : detail::MapLuaErrorCode(rc, vm_->LastError().text);
        return core::Result<void>::Fail(MakeError(code));
    }

    // ---- 结果：chunk 返回 table ⇒ 该表为模块表；否则以 env 为模块表 ----
    //
    // 【踩坑 · 栈序】`lua_pcall` 要求**被调函数在栈顶**。此前把 env 压在 chunk 之上，
    // pcall 便把 env（一张 table）当成被调函数，报 `attempt to call a table value`
    // —— 症状极具误导性：chunk 根本没执行，却报成「调用了一个 table」。
    // 因此本函数严格保证：env 先压栈、chunk 后压栈；`lua_setupvalue` 会弹出传入值，
    // pcall 前栈顶自然回到 chunk。
    int module = -1;
    if (lua_istable(state, -1) != 0) {
        if (lua_getmetatable(state, -1) == 0) {
            // 给返回的表挂 `__index = env`，使 Call 也能找到 env 里的函数。
            lua_createtable(state, 0, 1);
            (void)lua_pushvalue(state, env);
            lua_setfield(state, -2, "__index");
            lua_setmetatable(state, -2);
        } else {
            lua_pop(state, 1);  // 脚本自带元表：尊重脚本，不覆盖（文档已声明）
        }
        module = luaL_ref(state, LUA_REGISTRYINDEX);
    } else {
        (void)lua_pushvalue(state, env);
        module = luaL_ref(state, LUA_REGISTRYINDEX);
    }
    // 把实际使用的 `_ENV` 也固化成一个注册表引用返回给调用方（热更需要长期持有它）。
    (void)lua_pushvalue(state, env);
    *out_env_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    lua_settop(state, base);
    *out_module_ref = module;
    (void)name;
    return core::Result<void>::Ok();
}

core::Result<int> ScriptContext::RunChunkFromSource(std::string_view name,
                                                    std::string_view source, int env_ref,
                                                    int* out_env_ref) {
    int chunk_ref = -1;
    const core::Result<void> compiled = CompileChunk(name, source, &chunk_ref);
    if (!compiled) {
        return core::Result<int>::Fail(compiled.Err());
    }
    int module_ref = -1;
    int used_env_ref = -1;
    const core::Result<void> ran =
        RunCompiledChunk(name, chunk_ref, env_ref, &module_ref, &used_env_ref);
    luaL_unref(vm_->NativeState(), LUA_REGISTRYINDEX, chunk_ref);
    if (!ran) {
        // 失败路径：本次新建的 `_ENV` 引用必须立刻释放，否则每次失败的 Prepare/Activate
        // 都会在 registry 里漏一个槽位（§19「连续热更不泄漏」的反面）。
        if (used_env_ref >= 0 && used_env_ref != env_ref) {
            luaL_unref(vm_->NativeState(), LUA_REGISTRYINDEX, used_env_ref);
        }
        return core::Result<int>::Fail(ran.Err());
    }
    if (out_env_ref != nullptr) {
        *out_env_ref = used_env_ref;
    } else if (used_env_ref >= 0 && used_env_ref != env_ref) {
        luaL_unref(vm_->NativeState(), LUA_REGISTRYINDEX, used_env_ref);
    }
    return core::Result<int>::Ok(module_ref);
}

// ---------------------------------------------------------------------------
// §7 Load / Unload / Call
// ---------------------------------------------------------------------------

core::Result<ScriptId> ScriptContext::Load(std::string_view name, std::string_view source) {
    if (!vm_->OnOwnerThread()) {
        return core::Result<ScriptId>::Fail(core::Error(
            core::ErrorCode::BUSY, "not lua owner thread", core::domain::kLua));
    }
    if (name.empty()) {
        return core::Result<ScriptId>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "empty script name", core::domain::kLua));
    }

    // 同名脚本 → **热替换**（§13 热更语义）：id 保持不变，调用方持有旧 id 依旧有效。
    const auto existing = by_name_.find(name);
    const ScriptId target_id = existing != by_name_.end()
                                   ? existing->second
                                   : static_cast<ScriptId>(scripts_.size() + 1);

    if (run_depth_ > 0) {
        // 正在执行脚本：**禁止 Tick 中途替换**。先编译（语法错误立即反馈），
        // 再把变更暂存到 Tick 安全点生效（§13 Compile → Validate → Load → Safe Point → Activate）。
        int chunk_ref = -1;
        const core::Result<void> compiled = CompileChunk(name, source, &chunk_ref);
        if (!compiled) {
            return core::Result<ScriptId>::Fail(compiled.Err());
        }
        PendingOp op;
        op.kind = PendingOp::Kind::Load;
        op.id = target_id;
        op.name.assign(name);
        op.source.assign(source);
        op.chunk_ref = chunk_ref;
        pending_.push_back(std::move(op));
        return core::Result<ScriptId>::Ok(target_id);
    }

    int env_ref = -1;
    auto module = RunChunkFromSource(name, source, -1, &env_ref);
    if (!module) {
        return core::Result<ScriptId>::Fail(module.Err());
    }
    return ActivateScript(name, module.Value(), env_ref);
}

core::Result<ScriptId> ScriptContext::ReloadInPlace(std::string_view name,
                                                   std::string_view source) {
    if (!vm_->OnOwnerThread()) {
        return core::Result<ScriptId>::Fail(core::Error(
            core::ErrorCode::BUSY, "not lua owner thread", core::domain::kLua));
    }
    if (name.empty()) {
        return core::Result<ScriptId>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "empty script name", core::domain::kLua));
    }
    // §9「Activate 只能在 SimulationThread 的 Tick Safe Point」的强制点：
    // 正在执行脚本时调用 = 试图在 Tick 中途替换 → 拒绝（不排队；热更有自己的 Prepare/票证
    // 机制，宿主应在安全点重试）。
    if (run_depth_ > 0) {
        return core::Result<ScriptId>::Fail(core::Error(
            core::ErrorCode::BUSY, "reload not allowed during script execution",
            core::domain::kLua));
    }
    const auto existing = by_name_.find(name);
    if (existing == by_name_.end()) {
        // 热替换不是装载：名字不存在就是调用方搞错了，静默新建会掩盖问题。
        return core::Result<ScriptId>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "script not loaded", core::domain::kLua));
    }
    const ScriptId id = existing->second;
    if (id == kInvalidScriptId || id > scripts_.size() || scripts_[id - 1].module_ref < 0) {
        return core::Result<ScriptId>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "script not loaded", core::domain::kLua));
    }
    const int old_env_ref = scripts_[id - 1].env_ref;

    int env_ref = -1;
    auto module = RunChunkFromSource(name, source, old_env_ref, &env_ref);
    if (!module) {
        // 新版本编译/执行失败：**旧版本完全不受影响**（§20 验收 #2）。上面 RunChunkFromSource
        // 已负责释放本次新建的 env 引用，这里直接透传错误。
        return core::Result<ScriptId>::Fail(module.Err());
    }
    return ActivateScript(name, module.Value(), env_ref);
}

core::Result<ScriptId> ScriptContext::ActivateScript(std::string_view name, int module_ref,
                                                    int env_ref) {
    lua_State* state = vm_->NativeState();
    const auto existing = by_name_.find(name);
    if (existing != by_name_.end()) {
        ScriptEntry& entry = scripts_[existing->second - 1];
        if (entry.module_ref >= 0) {
            luaL_unref(state, LUA_REGISTRYINDEX, entry.module_ref);  // 热替换：释放旧模块表
        }
        if (entry.env_ref >= 0) {
            // 旧 `_ENV` 也必须释放：模块表与 `_ENV` 是两个独立的 registry 槽位，
            // 只放前者会在每次热更漏一个槽位（§19「连续热更不泄漏」的反面）。
            luaL_unref(state, LUA_REGISTRYINDEX, entry.env_ref);
        }
        entry.module_ref = module_ref;
        entry.env_ref = env_ref;
        entry.name.assign(name);
        vm_->BumpVersion();
        return core::Result<ScriptId>::Ok(entry.id);
    }

    ScriptEntry entry;
    entry.id = static_cast<ScriptId>(scripts_.size() + 1);
    entry.name.assign(name);
    entry.module_ref = module_ref;
    entry.env_ref = env_ref;
    scripts_.push_back(entry);
    by_name_.emplace(entry.name, entry.id);
    activated_.push_back(scripts_.size() - 1);
    vm_->BumpVersion();
    return core::Result<ScriptId>::Ok(entry.id);
}

core::Result<void> ScriptContext::Unload(ScriptId id) {
    if (!vm_->OnOwnerThread()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "not lua owner thread", core::domain::kLua));
    }
    if (run_depth_ > 0) {
        if (!IsLoaded(id)) {
            return core::Result<void>::Fail(core::Error(
                core::ErrorCode::NOT_FOUND, "script not loaded", core::domain::kLua));
        }
        PendingOp op;
        op.kind = PendingOp::Kind::Unload;
        op.id = id;
        pending_.push_back(std::move(op));
        return core::Result<void>::Ok();
    }
    return UnloadImmediate(id);
}

core::Result<void> ScriptContext::UnloadImmediate(ScriptId id) {
    if (!IsLoaded(id)) {
        // 幂等语义：重复卸载返回 NOT_FOUND（可安全重试，与 TASK-011 Destroy 同口径）。
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "script not loaded", core::domain::kLua));
    }
    ScriptEntry& entry = scripts_[id - 1];
    luaL_unref(vm_->NativeState(), LUA_REGISTRYINDEX, entry.module_ref);
    if (entry.env_ref >= 0) {
        luaL_unref(vm_->NativeState(), LUA_REGISTRYINDEX, entry.env_ref);  // 释放私有 `_ENV`
        entry.env_ref = -1;
    }
    entry.module_ref = -1;
    by_name_.erase(entry.name);
    vm_->BumpVersion();
    return core::Result<void>::Ok();
}

core::Result<void> ScriptContext::CallWithArgs(ScriptId id, std::string_view fn,
                                              const ScriptArgs& args) {
    if (!vm_->OnOwnerThread()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "not lua owner thread", core::domain::kLua));
    }
    if (args.Size() > ScriptArgs::kCapacity) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "too many script args", core::domain::kLua));
    }
    if (!IsLoaded(id)) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "script not loaded", core::domain::kLua));
    }

    lua_State* state = vm_->NativeState();
    const int base = lua_gettop(state);

    lua_pushcfunction(state, &LuaErrorHandler);
    const int msgh = lua_gettop(state);

    (void)lua_rawgeti(state, LUA_REGISTRYINDEX, scripts_[id - 1].module_ref);  // [msgh][tbl]
    if (lua_istable(state, -1) == 0) {
        lua_settop(state, base);
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "module table missing", core::domain::kLua));
    }
    // 用 gettable（非 rawget）以走 `__index` → 脚本私有 _ENV → 沙箱全局链。
    detail::PushKey(state, fn);
    (void)lua_gettable(state, -2);  // [msgh][tbl][fn]
    if (lua_isfunction(state, -1) == 0) {
        lua_settop(state, base);
        LuaVM::ErrorDetail detail;
        detail.code = ScriptError::RuntimeError;
        detail.text.assign("script function not found");
        vm_->SetLastError(std::move(detail));
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "script function not found", core::domain::kLua));
    }

    for (std::size_t i = 0; i < args.Size(); ++i) {
        detail::PushScriptValue(state, args.At(i));
    }

    int rc = LUA_OK;
    ScriptError forced = ScriptError::Ok;
    {
        RunGuard guard(*this, *vm_);
        rc = lua_pcall(state, static_cast<int>(args.Size()), 0, msgh);
        forced = ForcedReason(guard.Aborted(), guard.AbortReason(), guard.AllocationFailed());
    }

    if (rc != LUA_OK) {
        CaptureLuaError(*vm_, rc, forced, state);
        lua_settop(state, base);
        const ScriptError code =
            forced != ScriptError::Ok ? forced
                                      : detail::MapLuaErrorCode(rc, vm_->LastError().text);
        return core::Result<void>::Fail(MakeError(code));
    }
    lua_settop(state, base);
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// 绑定装载原语 + 统一入口
// ---------------------------------------------------------------------------

core::Result<void> ScriptContext::AddBinding(BindingDef def) {
    // 只有一个装载原语（InstallBindingEntry）：注册表 + Lua 侧入口一起装。
    // 双路径会导致「注册表里有、Lua 里没有」这种只在下游调用时才暴露的不一致，
    // 因此 AddBinding 不做第二套实现，直接前向到同一原语（§30 Correctness 优先）。
    return InstallBindingEntry(def.name, def.kind, def.fn, def.user, def.read_only,
                               def.description);
}

core::Result<void> ScriptContext::InstallBindingEntry(std::string_view name, BindingKind kind,                                                     NativeFn fn, void* user, bool read_only,
                                                     std::string_view description) {
    // 绑定名必须是 `ns.fn`：脚本侧就是 `ns.fn(...)`，注册表键与 Lua 路径保持一致，
    // 避免「注册名」与「Lua 里怎么调」两套说法漂移。
    const std::size_t dot = name.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "binding name must be ns.fn", core::domain::kLua));
    }
    const std::string_view ns = name.substr(0, dot);
    const std::string_view member = name.substr(dot + 1);
    if (ns.size() >= 32) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "binding namespace too long", core::domain::kLua));
    }
    char ns_buf[32];
    for (std::size_t i = 0; i < ns.size(); ++i) {
        ns_buf[i] = ns[i];
    }
    ns_buf[ns.size()] = '\0';

    BindingDef def;
    def.name.assign(name);
    def.kind = kind;
    def.fn = fn;
    def.user = user;
    def.read_only = read_only;
    def.description.assign(description);
    const core::Result<void> added = bindings_.Add(std::move(def));
    if (!added) {
        return added;
    }

    lua_State* state = vm_->NativeState();
    const int base = lua_gettop(state);

    (void)lua_getglobal(state, ns_buf);
    if (lua_istable(state, -1) == 0) {
        lua_pop(state, 1);
        lua_createtable(state, 0, 8);
        (void)lua_pushvalue(state, -1);
        lua_setglobal(state, ns_buf);
    }
    const int table = lua_gettop(state);

    // 闭包：upvalue 1 = 绑定名 → BindingEntry 据此路由。
    (void)lua_pushlstring(state, name.data(), name.size());
    lua_pushcclosure(state, &ScriptContext::BindingEntry, 1);
    detail::PushKey(state, member);
    lua_insert(state, -2);     // [table][member][closure]
    lua_rawset(state, table);  // table[member] = closure

    lua_settop(state, base);
    return core::Result<void>::Ok();
}

int ScriptContext::BindingEntry(lua_State* state) noexcept {
    ScriptContext* ctx = ContextOf(state);
    std::size_t name_len = 0;
    const char* name_ptr = ctx != nullptr ? lua_tolstring(state, lua_upvalueindex(1), &name_len)
                                          : nullptr;
    if (ctx == nullptr || name_ptr == nullptr) {
        // 不可达（Create 时已登记 context，装载时已设 upvalue）；留个明确错误比静默更安全。
        detail::RaiseLuaError(state, "lua: binding entry misconfigured");
    }

    char err_buf[192];
    std::size_t err_len = 0;
    int nres = 0;
    bool ok = false;
    {
        // **作用域纪律**：这里所有非平凡对象（Result / ScriptCall 内的容器）都必须在
        // 下面 lua_error（longjmp）之前析构干净，否则会跳过析构 —— 见文件头说明。
        const std::string_view name(name_ptr, name_len);
        const int arg_count = lua_gettop(state);
        ScriptCall call(*ctx, name, detail::Erase(state), 1,
                        static_cast<std::size_t>(arg_count), ctx->services_);
        const core::Result<const BindingDef*> found = ctx->bindings_.Find(name);
        if (!found) {
            err_len = detail::CopyErrorText(found.Err(), err_buf, sizeof(err_buf));
        } else {
            const BindingDef* def = found.Value();
            const core::Result<int> outcome = def->fn(call, def->user);
            if (outcome) {
                nres = outcome.Value();
                ok = true;
            } else {
                err_len = detail::CopyErrorText(outcome.Err(), err_buf, sizeof(err_buf));
            }
        }
    }
    if (ok) {
        return nres;
    }
    (void)lua_pushlstring(state, err_buf, err_len);
    return lua_error(state);
}

// ---------------------------------------------------------------------------
// 事件桥：名字 ↔ 具体 C++ 事件类型
// ---------------------------------------------------------------------------

core::Result<void> ScriptContext::RegisterEventBridge(std::string_view name,
                                                     EventBridgeInstall install, void* user) {
    if (name.empty() || install == nullptr) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "invalid event bridge", core::domain::kLua));
    }
    for (const EventBridge& existing : event_bridges_) {
        if (existing.name == name) {
            return core::Result<void>::Fail(core::Error(
                core::ErrorCode::INVALID_ARGUMENT, "duplicate event bridge", core::domain::kLua));
        }
    }
    event_bridges_.push_back(EventBridge{std::string(name), install, user});
    return core::Result<void>::Ok();
}

core::Result<void> ScriptContext::EnsureEventSubscription(std::string_view name) {
    for (const std::string& installed : installed_events_) {
        if (installed == name) {
            return core::Result<void>::Ok();  // 幂等：同一事件名只装一次 C++ 订阅
        }
    }
    core::EventBus* bus = services_.events;
    if (bus == nullptr) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "event api not bound", core::domain::kLua));
    }
    for (const EventBridge& bridge : event_bridges_) {
        if (bridge.name != name) {
            continue;
        }
        auto sub = bridge.install(*bus, *this, name, bridge.user);
        if (!sub) {
            return core::Result<void>::Fail(sub.Err());
        }
        bridge_subs_.push_back(sub.Value());
        break;
    }
    // 未命中桥的名字无需额外 C++ 订阅：BindEventApi 装的通用 `ScriptEvent` 订阅会覆盖它。
    installed_events_.emplace_back(name);
    return core::Result<void>::Ok();
}

void ScriptContext::AddSubscriber(std::string_view name, int fn_ref) {
    Subscriber entry;
    entry.sub_id = ++next_sub_id_;
    entry.fn_ref = fn_ref;
    const std::size_t index = subscribers_.size();
    subscribers_.push_back(entry);
    subs_by_name_[std::string(name)].push_back(index);
}

std::size_t ScriptContext::SubscriberCount(std::string_view name) const noexcept {
    const auto it = subs_by_name_.find(name);
    return it == subs_by_name_.end() ? 0 : it->second.size();
}

core::Result<std::size_t> ScriptContext::DispatchEventToSubscribers(
    std::string_view name, void (*fill)(TableWriter&, const void*), const void* ev) {
    if (!vm_->OnOwnerThread()) {
        return core::Result<std::size_t>::Fail(core::Error(
            core::ErrorCode::BUSY, "not lua owner thread", core::domain::kLua));
    }
    const auto it = subs_by_name_.find(name);
    if (it == subs_by_name_.end() || it->second.empty()) {
        // 无订阅者不是错误：调用方（多为 Drain 路径）不该因此被背压。
        return core::Result<std::size_t>::Ok(0);
    }

    lua_State* state = vm_->NativeState();
    const int base = lua_gettop(state);
    lua_pushcfunction(state, &LuaErrorHandler);
    const int msgh = lua_gettop(state);
    lua_createtable(state, 0, 8);
    const int table = lua_gettop(state);
    if (fill != nullptr) {
        TableWriter writer(detail::Erase(state), table);
        fill(writer, ev);
    }

    std::size_t delivered = 0;
    for (const std::size_t index : it->second) {
        if (index >= subscribers_.size() || subscribers_[index].fn_ref < 0) {
            continue;
        }
        lua_settop(state, table);  // 清掉上一轮残留（含上一轮的错误对象）
        (void)lua_rawgeti(state, LUA_REGISTRYINDEX, subscribers_[index].fn_ref);
        lua_pushvalue(state, table);

        int rc = LUA_OK;
        bool aborted = false;
        ScriptError reason = ScriptError::Ok;
        {
            RunGuard guard(*this, *vm_);
            rc = lua_pcall(state, 1, 0, msgh);
            aborted = guard.Aborted();
            reason = guard.AbortReason();
        }
        if (rc != LUA_OK) {
            // 派发隔离（§19）：单个订阅者出错/超限只记录，其余订阅者照常收到。
            CaptureLuaError(*vm_, rc, aborted ? reason : ScriptError::Ok, state);
            lua_settop(state, table);
            continue;
        }
        ++delivered;
    }
    lua_settop(state, base);
    return core::Result<std::size_t>::Ok(delivered);
}

// ---------------------------------------------------------------------------
// §13 Tick Safe Point
// ---------------------------------------------------------------------------

core::Result<void> ScriptContext::ApplyPendingChanges() {
    if (!vm_->OnOwnerThread()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "not lua owner thread", core::domain::kLua));
    }
    if (run_depth_ > 0) {
        // 严格禁止 Tick 中途热切（§13 / §21 明令）。
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "cannot apply during script execution", core::domain::kLua));
    }
    if (pending_.empty()) {
        return core::Result<void>::Ok();
    }

    std::vector<PendingOp> ops;
    ops.swap(pending_);  // 取出后清空：应用期间若再入队，不会自我干扰

    lua_State* state = vm_->NativeState();
    for (PendingOp& op : ops) {
        if (op.kind == PendingOp::Kind::Unload) {
            (void)UnloadImmediate(op.id);  // 已被卸载等情形按幂等语义忽略
            continue;
        }
        int module_ref = -1;
        int env_ref = -1;
        const core::Result<void> ran =
            RunCompiledChunk(op.name, op.chunk_ref, -1, &module_ref, &env_ref);
        if (op.chunk_ref >= 0) {
            luaL_unref(state, LUA_REGISTRYINDEX, op.chunk_ref);
            op.chunk_ref = -1;
        }
        if (!ran) {
            // 激活失败（例如运行期错误 / 内存触顶）：**放弃该变更**，保留原脚本集
            // —— 这是 §13 Rollback 的语义：热更不能把好的旧版本一起赔进去。
            if (env_ref >= 0) {
                luaL_unref(state, LUA_REGISTRYINDEX, env_ref);
            }
            continue;
        }
        (void)ActivateScript(op.name, module_ref, env_ref);
    }
    return core::Result<void>::Ok();
}

}  // namespace mmo::script
