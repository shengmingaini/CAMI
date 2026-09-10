// scripting/lua/src/lua_vm.cpp —— TASK-031 · LuaVM（§15.2 / §15.4 / §15.5 / §15.6 / §15.9）。
//
// 本文件集中处理三件事，全部与 Lua C API 的**版本差异**和**错误语义**有关：
//   1. 计数 allocator：把内存上限做成硬约束（分配失败 → LUA_ERRMEM → MemoryLimit）；
//   2. debug hook：指令计数 + 挂钟检查 + 栈深检查**三合一**，超限即经 lua_error 中止；
//   3. 错误映射：Lua 错误码/消息 → ScriptError（含行号解析）。
//
// Lua 5.5 兼容性（**实测踩过，务必保留版本分支**）
// ---------------------------------------------
//   Lua 5.5.1 的 `lua_newstate` 签名变更为 `(lua_Alloc, void*, unsigned seed)` ——
//   比 5.4 多一个 seed 参数。直接按 5.4 写会在 5.5 上编译失败（参数个数不匹配）。
//   故此处用 `LUA_VERSION_NUM >= 505` 分支，两个版本都能编。
//   另：5.5 新增 `luaL_openselectedlibs`（白名单位掩码开库），见 sandbox.cpp。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "internal.h"

#include "mmo/core/time/clock.h"
#include "mmo/script/lua_vm.h"

namespace mmo::script {
namespace {

/// 确定性种子（§25 Determinism / Replay Test 的前提）。
///
/// 固定种子 ⇒ 同一脚本 + 同一输入在不同 VM 实例上产生**逐位相同**的行为
/// （含 `pairs` 的 table 遍历顺序），这是 Replay / 确定性校验可用的前提。
/// 代价：字符串哈希可被预测，理论上可构造哈希碰撞攻击；但玩法脚本由运营方部署
/// （可信来源，不是玩家上传），故不构成攻击面 —— 详见 docs/SANDBOX.md §5。
constexpr unsigned kDeterministicSeed = 0x9E3779B9u;

/// 解析 `"chunkname:line: text"` 中的脚本名与行号。
///
/// 为什么要自己解析：Lua 对运行期错误与语法错误都会把位置**前缀**进错误串
/// （`"test.lua:12: attempt to index a nil value"`），因此不需要额外 hook 就能拿到行号；
/// 只有「在 C 函数里主动 raise」的消息没有前缀，这时由 message handler 用
/// `luaL_where` 补上（见 LuaErrorHandler）。两种来源格式一致，故只需一个解析器。
bool FindPosition(std::string_view message, std::string_view* out_script, int* out_line) noexcept {
    for (std::size_t i = 0; i + 2 < message.size(); ++i) {
        if (message[i] != ':') {
            continue;
        }
        std::size_t j = i + 1;
        if (message[j] < '0' || message[j] > '9') {
            continue;
        }
        long value = 0;
        while (j < message.size() && message[j] >= '0' && message[j] <= '9') {
            if (value < 1000000) {
                value = value * 10 + static_cast<long>(message[j] - '0');
            }
            ++j;
        }
        if (j < message.size() && message[j] == ':') {
            if (out_script != nullptr) {
                *out_script = message.substr(0, i);
            }
            if (out_line != nullptr) {
                *out_line = static_cast<int>(value);
            }
            return true;
        }
    }
    return false;
}

/// `panic` 兜底：只有「保护调用之外」的致命错误才会走到这里（正常路径不可达，
/// 因为所有入口 Load/Call/沙箱装配都在 `lua_pcall` 之内，见 §19「宿主进程不崩溃」）。
/// 这里只把消息写到 stderr；Lua 在 panic 返回后会 abort，无法避免，故记录以便定位。
int LuaPanic(lua_State* state) {
    const char* msg = lua_tostring(state, -1);
    if (msg != nullptr) {
        const std::size_t len = std::strlen(msg);
        (void)std::fwrite("lua panic: ", 1, 11, stderr);
        (void)std::fwrite(msg, 1, len, stderr);
        (void)std::fwrite("\n", 1, 1, stderr);
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// debug hook 的访问桥：三合一限制检查
//
// 必须是 mmo::script::detail::HookAccess（lua_vm.h 里 friend 的就是它），
// 因此本块不能放进上面那个匿名 namespace。
// ---------------------------------------------------------------------------

namespace detail {

struct HookAccess {
    /// 每次 hook 回调执行一次：指令计数 + 栈深 + 挂钟。
    ///
    /// 三者的优先级刻意按「代价从低到高」排列：先扣指令预算（纯算术），再查栈深
    /// （O(depth) 指针跳转，depth ≤ 64），最后才读时钟（最贵）。任一超限立即
    /// `lua_error` 中止 —— 这是 §19「脚本死循环不卡死 Tick」的实现点。
    static void OnCheck(LuaVM& vm, lua_State* state) {
        vm.instr_checks_.fetch_add(1, std::memory_order_relaxed);

        LuaVM::ExecBudget& budget = vm.budget_;
        if (!budget.active) {
            // 不在受控执行区间（例如宿主手工驱动 Lua）；只计数，不施加预算。
            return;
        }

        const std::uint32_t period = vm.hook_period_ == 0 ? 1u : vm.hook_period_;

        // ---- 1) 指令预算 ----
        if (budget.instructions_left <= period) {
            budget.instructions_left = 0;
            budget.aborted = true;
            budget.reason = ScriptError::InstructionLimit;
            vm.limit_hits_.fetch_add(1, std::memory_order_relaxed);
            RaiseLuaError(state, "lua: instruction limit exceeded");
        }
        budget.instructions_left -= period;

        // ---- 2) 调用栈深度（防无限递归打爆 C 栈）----
        // lua_getstack(L, level) 返回非 0 表示「存在 depth >= level 的帧」。
        lua_Debug frame{};
        if (lua_getstack(state, static_cast<int>(vm.limits_.max_stack_depth), &frame) != 0) {
            budget.aborted = true;
            budget.reason = ScriptError::StackOverflow;
            vm.limit_hits_.fetch_add(1, std::memory_order_relaxed);
            RaiseLuaError(state, "lua: stack depth limit exceeded");
        }

        // ---- 3) 挂钟预算（与指令预算双保险，§15.5）----
        if (core::MonotonicClock::Now() >= budget.deadline_ns) {
            budget.aborted = true;
            budget.reason = ScriptError::Timeout;
            vm.limit_hits_.fetch_add(1, std::memory_order_relaxed);
            RaiseLuaError(state, "lua: execution time limit exceeded");
        }
    }
};

bool ParseScriptPosition(std::string_view message, std::string_view* out_script,
                         int* out_line) noexcept {
    return FindPosition(message, out_script, out_line);
}

ScriptError MapLuaErrorCode(int lua_rc, std::string_view message) noexcept {
    switch (lua_rc) {
        case LUA_ERRSYNTAX:
            return ScriptError::CompileError;
        case LUA_ERRMEM:
            return ScriptError::MemoryLimit;
        case LUA_ERRERR:
            return ScriptError::RuntimeError;
        case LUA_ERRRUN:
        default:
            break;
    }
    // "stack overflow" / "C stack overflow" 都归 StackOverflow —— 两者都表示递归失控。
    const bool stack_overflow =
        message.find("stack overflow") != std::string_view::npos;
    return stack_overflow ? ScriptError::StackOverflow : ScriptError::RuntimeError;
}

}  // namespace detail

namespace {

/// 文件内静态 hook：从 allocator 的 ud 取回 LuaVM，再转交访问桥。
///
/// 用 allocator 的 ud 而不是 `lua_getextraspace` —— `lua_getallocf` 是普通函数调用，
/// 不像 `lua_getextraspace` 那样展开出 C 风格强制转换（会被 -Wold-style-cast 命中）。
void LuaDebugHook(lua_State* state, lua_Debug* ar) {
    (void)ar;
    void* ud = nullptr;
    (void)lua_getallocf(state, &ud);
    auto* vm = static_cast<LuaVM*>(ud);
    if (vm == nullptr) {
        return;
    }
    detail::HookAccess::OnCheck(*vm, state);
}

}  // namespace

// ---------------------------------------------------------------------------
// ScriptError 名称化与错误映射
// ---------------------------------------------------------------------------

const char* ToString(ScriptError err) noexcept {
    switch (err) {
        case ScriptError::Ok:
            return "Ok";
        case ScriptError::CompileError:
            return "CompileError";
        case ScriptError::RuntimeError:
            return "RuntimeError";
        case ScriptError::MemoryLimit:
            return "MemoryLimit";
        case ScriptError::InstructionLimit:
            return "InstructionLimit";
        case ScriptError::Timeout:
            return "Timeout";
        case ScriptError::StackOverflow:
            return "StackOverflow";
        case ScriptError::SandboxViolation:
            return "SandboxViolation";
    }
    return "UNKNOWN";
}

core::Error ToCoreError(ScriptError err) noexcept {
    switch (err) {
        case ScriptError::Ok:
            return core::Error(core::ErrorCode::OK, "ok", core::domain::kLua);
        case ScriptError::CompileError:
            return core::Error(core::ErrorCode::INVALID_ARGUMENT, "script compile error",
                               core::domain::kLua);
        case ScriptError::RuntimeError:
            return core::Error(core::ErrorCode::INTERNAL_ERROR, "script runtime error",
                               core::domain::kLua);
        case ScriptError::MemoryLimit:
            return core::Error(core::ErrorCode::INTERNAL_ERROR, "script memory limit",
                               core::domain::kLua);
        case ScriptError::StackOverflow:
            return core::Error(core::ErrorCode::INTERNAL_ERROR, "script stack overflow",
                               core::domain::kLua);
        case ScriptError::InstructionLimit:
            return core::Error(core::ErrorCode::TIMEOUT, "script instruction limit",
                               core::domain::kLua);
        case ScriptError::Timeout:
            return core::Error(core::ErrorCode::TIMEOUT, "script exec timeout", core::domain::kLua);
        case ScriptError::SandboxViolation:
            return core::Error(core::ErrorCode::UNAUTHORIZED, "script sandbox violation",
                               core::domain::kLua);
    }
    return core::Error(core::ErrorCode::INTERNAL_ERROR, "unknown script error",
                       core::domain::kLua);
}

// ---------------------------------------------------------------------------
// LuaVM 生命周期
// ---------------------------------------------------------------------------

LuaVM::LuaVM(const LuaLimits& limits) noexcept
    : limits_(limits), owner_thread_(std::this_thread::get_id()) {}

LuaVM::~LuaVM() {
    if (state_ != nullptr) {
        // lua_close 会通过本对象自己的 allocator 归还全部内存（mem_used_ 归零），
        // 因此必须在成员析构之前执行 —— 这里是析构函数体，满足该前提。
        lua_close(state_);
        state_ = nullptr;
    }
}

core::Result<std::unique_ptr<LuaVM>> LuaVM::Create(const LuaLimits& limits) {
    if (limits.memory_bytes == 0 || limits.max_instructions == 0 ||
        limits.max_stack_depth == 0 || limits.max_exec_time.count() <= 0) {
        return core::Result<std::unique_ptr<LuaVM>>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "invalid lua limits", core::domain::kLua));
    }

    std::unique_ptr<LuaVM> vm(new LuaVM(limits));

    lua_State* state = nullptr;
#if defined(LUA_VERSION_NUM) && (LUA_VERSION_NUM >= 505)
    // Lua 5.5：lua_newstate(lua_Alloc, void* ud, unsigned seed)
    state = lua_newstate(&LuaVM::Allocate, static_cast<void*>(vm.get()), kDeterministicSeed);
#else
    // Lua 5.1–5.4：lua_newstate(lua_Alloc, void* ud)
    state = lua_newstate(&LuaVM::Allocate, static_cast<void*>(vm.get()));
#endif
    if (state == nullptr) {
        return core::Result<std::unique_ptr<LuaVM>>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "lua_newstate failed", core::domain::kLua));
    }
    vm->state_ = state;
    (void)lua_atpanic(state, &LuaPanic);
    InstallHook(state, vm->hook_period_);

    const core::Result<void> sandbox = vm->OpenSandbox();
    if (!sandbox) {
        // unique_ptr 析构 → lua_close → 内存统计归零，不留残留。
        return core::Result<std::unique_ptr<LuaVM>>::Fail(sandbox.Err());
    }
    InstallHook(state, vm->hook_period_);  // 沙箱装配后重申 hook（防被库覆盖）
    return core::Result<std::unique_ptr<LuaVM>>::Ok(std::move(vm));
}

// ---------------------------------------------------------------------------
// 计数 allocator（§15.2 / §15.4）
// ---------------------------------------------------------------------------

void* LuaVM::Allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize) noexcept {
    auto* vm = static_cast<LuaVM*>(ud);
    if (vm == nullptr) {
        return nullptr;
    }

    // Lua 约定：nsize == 0 即释放（ptr 可能为 nullptr，此时是空操作）。
    if (nsize == 0) {
        if (ptr != nullptr) {
            std::free(ptr);
            vm->mem_used_.fetch_sub(osize, std::memory_order_relaxed);
        }
        return nullptr;
    }

    // 「本次分配后是否超上限」必须在真正分配前判定：这是把内存上限做成**硬约束**
    // 的关键 —— 不允许「先超再回收」。判定失败直接返回 nullptr，Lua 会抛 LUA_ERRMEM
    // 并由 pcall 转成 MemoryLimit（VM 状态保持一致，可继续跑其它脚本，§19）。
    const std::size_t used = vm->mem_used_.load(std::memory_order_relaxed);
    const std::size_t base = (ptr != nullptr && used >= osize) ? used - osize : used;
    if (base + nsize > vm->limits_.memory_bytes) {
        vm->alloc_fails_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    void* next = ptr != nullptr ? std::realloc(ptr, nsize) : std::malloc(nsize);
    if (next == nullptr) {
        // 平台分配失败：**不改记账**（否则会与 Lua 的实际持有量脱钩）。
        vm->alloc_fails_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    const std::size_t now = base + nsize;
    vm->mem_used_.store(now, std::memory_order_relaxed);
    vm->alloc_calls_.fetch_add(1, std::memory_order_relaxed);
    std::size_t peak = vm->mem_peak_.load(std::memory_order_relaxed);
    while (now > peak) {
        if (vm->mem_peak_.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
            break;
        }
    }
    return next;
}

// ---------------------------------------------------------------------------
// hook 安装与统计
// ---------------------------------------------------------------------------

void LuaVM::InstallHook(lua_State* state, std::uint32_t period) {
    const int count = period == 0 ? 1 : static_cast<int>(period);
    lua_sethook(state, &LuaDebugHook, LUA_MASKCOUNT, count);
}

void LuaVM::SetHookPeriod(std::uint32_t period) noexcept {
    hook_period_ = period == 0 ? 1u : period;
    if (state_ != nullptr) {
        InstallHook(state_, hook_period_);
    }
}

bool LuaVM::OnOwnerThread() const noexcept {
    return std::this_thread::get_id() == owner_thread_;
}

std::size_t LuaVM::MemoryUsed() const noexcept {
    return mem_used_.load(std::memory_order_relaxed);
}
std::size_t LuaVM::PeakMemory() const noexcept {
    return mem_peak_.load(std::memory_order_relaxed);
}
std::size_t LuaVM::AllocCalls() const noexcept {
    return alloc_calls_.load(std::memory_order_relaxed);
}
std::size_t LuaVM::AllocFailCalls() const noexcept {
    return alloc_fails_.load(std::memory_order_relaxed);
}
LuaMemStats LuaVM::MemStats() const noexcept {
    LuaMemStats stats;
    stats.used = MemoryUsed();
    stats.peak = PeakMemory();
    stats.limit = limits_.memory_bytes;
    stats.alloc_calls = AllocCalls();
    stats.fail_calls = AllocFailCalls();
    return stats;
}
std::size_t LuaVM::LimitHits() const noexcept {
    return limit_hits_.load(std::memory_order_relaxed);
}
std::size_t LuaVM::InstructionChecks() const noexcept {
    return instr_checks_.load(std::memory_order_relaxed);
}
std::uint32_t LuaVM::Version() const noexcept {
    return version_.load(std::memory_order_relaxed);
}
void LuaVM::BumpVersion() noexcept {
    version_.fetch_add(1, std::memory_order_relaxed);
}

void LuaVM::SetLastError(ErrorDetail detail) noexcept {
    // ErrorDetail 内含 std::string，赋值可能分配；本函数在错误路径（Cold Path）调用。
    // 标记 noexcept 是接口约定：分配失败时 std::terminate 也好过让错误状态半残。
    last_error_ = std::move(detail);
}

std::size_t LuaVM::CopyErrorMessage(const core::Error& err, char* out, std::size_t cap) noexcept {
    return detail::CopyErrorText(err, out, cap);
}

// ---------------------------------------------------------------------------
// 执行区间（§19 预算不复用 / §15.4 回滚）
// ---------------------------------------------------------------------------

LuaVM::ExecScope::ExecScope(LuaVM& vm) noexcept : vm_(vm) {
    saved_ = vm_.budget_;
    LuaVM::ExecBudget fresh{};
    fresh.active = true;
    fresh.instructions_left = vm_.limits_.max_instructions;
    // 每次执行独立计时：deadline 从本次进入时起算，绝不复用上一次的残余预算。
    constexpr core::SteadyNs kNanosPerMs = 1'000'000;
    fresh.deadline_ns = core::MonotonicClock::Now() +
                        static_cast<core::SteadyNs>(vm_.limits_.max_exec_time.count()) *
                            kNanosPerMs;
    vm_.budget_ = fresh;
}

LuaVM::ExecScope::~ExecScope() { vm_.budget_ = saved_; }

}  // namespace mmo::script
