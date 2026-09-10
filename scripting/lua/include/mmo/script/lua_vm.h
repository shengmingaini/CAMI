#pragma once

/// TASK-031 · LuaVM —— Lua 运行时实例（§7 LuaLimits / ScriptError，§9 线程模型，§15.2 / §15.4 / §15.5）。
///
/// 职责：lua_State 的创建与销毁、**自定义计数 allocator**（走限额与统计）、沙箱白名单开库、
/// 四类限制（内存 / 指令 / 时间 / 栈深）的强制、脚本错误 → mmo::core::Error 的映射。
///
/// 不透明状态约定（重要）
/// --------------------
/// 本头**不 include lua.h**：Lua 的 `lua_State` 只作前置声明出现，Lua 版本细节被关在本模块
/// `src/` 内。好处：下游模块不必耦合 Lua 版本、也不会误用裸 lua_State 破坏沙箱。
/// 只有本模块 `src/` 与需要写原生绑定的**宿主**才需要 `#include <lua.h>` 并调用 `NativeState()`。
///
/// 线程模型（§9 硬约束）
/// --------------------
/// 一个 LuaVM 绑定**一个创建线程**（`OwnerThread()`）。任何从其它线程发起的入口调用都会被
/// 拒绝并返回 `BUSY`，而不是产生数据竞争 —— 见 §20.1「每 Scene 一个 VM，不跨线程共享」。
/// LuaVM 自身不创建线程（§21 Forbidden）。
///
/// 热路径 / Cold Path
/// ------------------
/// **Hot Path**（§10 / §14）：真实脚本调用（`Call`）在 Tick 内逐次发生（技能公式、Buff 公式），
/// 因此执行路径禁止 IO / 同步远端调用 / 无界分配；本头的 `MemoryUsed()` 等统计接口全部
/// `noexcept` + 原子读，可安全在热路径采集。
/// **Cold Path**：`Create` / `OpenSandbox` / `Load` 只在启动期与热更 Safe Point 调用。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"

/// 前置声明：与 lua.h 的 `typedef struct lua_State lua_State;` 兼容。
/// 本模块公开头不引入 lua.h，从而使下游不必耦合具体 Lua 版本。
struct lua_State;

namespace mmo::script {

namespace detail {
/// debug hook 的访问桥（定义在 src/lua_vm.cpp）。仅用于让文件内静态 hook 访问
/// LuaVM 的执行预算，避免把预算字段暴露到公开接口。
struct HookAccess;
}  // namespace detail

/// §7 LuaLimits —— 单个 LuaVM 的资源上限（**字段冻结**，禁止删改；新增字段须走 version
/// + 兼容性评估，§27.1）。
///
/// 每一档都有明确的执行语义：
///   - `memory_bytes`    ：计数 allocator 的硬上限，超出即分配失败 → MemoryLimit；
///   - `max_instructions`：单次 Load/Call 的 Lua 指令预算，由 debug hook 计数；
///   - `max_exec_time`   ：单次 Load/Call 的挂钟预算，与指令预算**双保险**（§15.5）；
///   - `max_stack_depth` ：Lua 调用栈深度上限，防无限递归打爆 C 栈（§15.6）；
///   - `allow_io`        ：**恒为 false 生效**：io/os 库根本不开库（白名单，§21）；本字段
///                         保留为显式声明位，置 true 也不改变白名单（见 SANDBOX.md）。
///   - `allow_loadstring`：同上，`load` / `loadfile` / `dofile` 不在白名单内。
struct LuaLimits {
    std::size_t memory_bytes{8u * 1024u * 1024u};  ///< 单 VM 内存上限（默认 8MiB）
    std::uint32_t max_instructions{10'000'000u};   ///< 单次调用指令上限
    core::DurationMs max_exec_time{5};             ///< 单次调用时间上限（毫秒）
    std::uint32_t max_stack_depth{64u};            ///< 防无限递归
    bool allow_io{false};                          ///< 沙箱：禁 io/os 库
    bool allow_loadstring{false};                  ///< 沙箱：禁 load/loadfile/dofile
};

/// §7 ScriptError —— 脚本侧错误分类。与 `core::ErrorCode` 的映射见 `ToCoreError`。
enum class ScriptError : std::uint8_t {
    Ok = 0,
    CompileError,       ///< 语法错误 / 加载失败（含 luaL_loadbufferx 报错）
    RuntimeError,       ///< 运行期 error()、类型错误、索引 nil 等
    MemoryLimit,        ///< 计数 allocator 触顶（LUA_ERRMEM）
    InstructionLimit,   ///< 指令预算耗尽（debug hook 计数）
    Timeout,            ///< 挂钟预算耗尽
    StackOverflow,      ///< 调用栈深度超限 / C stack overflow
    SandboxViolation,   ///< 试图访问沙箱外能力（白名单外符号）
};

/// 名称化（日志 / 报告用）；未知值返回 "UNKNOWN"，禁止崩溃。
const char* ToString(ScriptError err) noexcept;

/// §7 `ToCoreError` —— ScriptError → core::Error（domain = `core::domain::kLua`）。
///
/// 映射表（ErrorCode 只有 9 个值，脚本错误按**处置语义**收敛，细分靠 message + ScriptError）：
///   Ok               -> OK               （调用方应先判 `ScriptError`，本函数返回 OK 视为传入有误）
///   CompileError     -> INVALID_ARGUMENT （脚本本身有问题，重试无意义）
///   RuntimeError     -> INTERNAL_ERROR
///   MemoryLimit      -> INTERNAL_ERROR
///   StackOverflow    -> INTERNAL_ERROR
///   InstructionLimit -> TIMEOUT          （预算耗尽 = 该次执行超时；可重试）
///   Timeout          -> TIMEOUT
///   SandboxViolation -> UNAUTHORIZED     （越权访问被拒）
core::Error ToCoreError(ScriptError err) noexcept;

/// 计数 allocator 的记账快照（供 benchmark / 报告取数，§18）。
struct LuaMemStats {
    std::size_t used{0};      ///< 当前占用字节
    std::size_t peak{0};      ///< 峰值占用字节
    std::size_t limit{0};     ///< 上限
    std::size_t alloc_calls{0};
    std::size_t fail_calls{0};  ///< 因触顶而失败的分配次数
};

/// Lua VM 实例。**每 Scene 一个**，禁止全局单例（§21）；生命周期由 `unique_ptr` 拥有。
class LuaVM {
public:
    /// 创建 VM：`lua_newstate` + 计数 allocator + 打开白名单沙箱库。
    /// 失败（分配失败 / 库打开异常）返回 INTERNAL_ERROR，绝不抛异常。
    static core::Result<std::unique_ptr<LuaVM>> Create(const LuaLimits& limits);

    ~LuaVM();
    LuaVM(const LuaVM&) = delete;
    LuaVM& operator=(const LuaVM&) = delete;
    LuaVM(LuaVM&&) = delete;
    LuaVM& operator=(LuaVM&&) = delete;

    // ------------------------------------------------------------------
    // 不透明状态（仅本模块 src/ 与需要写原生绑定的宿主使用）
    // ------------------------------------------------------------------

    /// 裸 lua_State*（生命周期由本对象拥有，**禁止**调用方 lua_close）。
    /// 普通用法（Create / Load / Call / 五项绑定）不需要它。
    lua_State* NativeState() noexcept { return state_; }

    // ------------------------------------------------------------------
    // §9 线程归属
    // ------------------------------------------------------------------

    std::thread::id OwnerThread() const noexcept { return owner_thread_; }
    bool OnOwnerThread() const noexcept;

    // ------------------------------------------------------------------
    // 限额与统计（全部 noexcept + 原子读：可安全在热路径采集，§10）
    // ------------------------------------------------------------------

    const LuaLimits& Limits() const noexcept { return limits_; }
    std::size_t MemoryUsed() const noexcept;
    std::size_t PeakMemory() const noexcept;
    std::size_t MemoryLimit() const noexcept { return limits_.memory_bytes; }
    std::size_t AllocCalls() const noexcept;
    std::size_t AllocFailCalls() const noexcept;
    LuaMemStats MemStats() const noexcept;
    /// 四类限制被触发的累计次数（观测用）。
    std::size_t LimitHits() const noexcept;
    /// debug hook 被调用的累计次数（指令计数开销的分子，§18 `instruction_check_ns`）。
    std::size_t InstructionChecks() const noexcept;

    /// 脚本版本（§13：必须在脚本切换后递增，供下游判断是否需重建缓存）。
    std::uint32_t Version() const noexcept;
    void BumpVersion() noexcept;

    /// 指令检查周期：每 `period` 条 Lua 指令回调一次 hook。
    ///
    /// 只影响**中止粒度**，不影响是否会中止 —— 超限仍会在下一次检查时立即中止；
    /// 调大只是让中止晚几毫秒、检查开销更低。默认 1000。
    /// 仅供 benchmark（测量单次检查开销）与内部调优使用，禁止用于放宽安全性。
    void SetHookPeriod(std::uint32_t period) noexcept;
    std::uint32_t HookPeriod() const noexcept { return hook_period_; }

    /// 沙箱是否已打开（白名单开库是否成功）。
    bool SandboxOpen() const noexcept { return sandbox_open_; }

    /// 执行区间 RAII（**定义在 src/lua_vm.cpp**）。
    ///
    /// 声明放在 public 的原因：同模块 `src/`（ScriptContext 的 RunGuard）需要**按值**持有它。
    /// 下游模块不需要也不应直接使用 —— 用 `ScriptContext::Call` / `Load` 即可，
    /// 那里已自动装载预算与回滚语义。
    class ExecScope;

    // ------------------------------------------------------------------
    // 详细错误（§15.9：错误映射含行号与脚本名）
    // ------------------------------------------------------------------

    struct ErrorDetail {
        ScriptError code{ScriptError::Ok};
        std::string text;    ///< Lua 原始错误文本（已剥掉位置前缀）
        std::string script;  ///< 脚本名（chunkname）
        int line{0};         ///< 出错行号（0 = 未知）
    };

    const ErrorDetail& LastError() const noexcept { return last_error_; }
    void SetLastError(ErrorDetail detail) noexcept;

    /// 把 `core::Error` 的消息文本复制成**平凡类型**缓冲，供绑定函数在 longjmp 前使用。
    static std::size_t CopyErrorMessage(const core::Error& err, char* out, std::size_t cap) noexcept;

private:
    explicit LuaVM(const LuaLimits& limits) noexcept;

    friend struct detail::HookAccess;

    /// 打开白名单沙箱（§15.7 / §21：白名单而非黑名单）。实现见 src/sandbox.cpp。
    core::Result<void> OpenSandbox();

    /// 计数 allocator（`lua_Alloc` 签名，实现见 src/lua_vm.cpp）。
    ///
    /// 为什么在**分配前**判定上限：这是把 `memory_bytes` 做成硬约束的关键 ——
    /// 不允许「先超再回收」。判定失败返回 nullptr，Lua 抛 LUA_ERRMEM → MemoryLimit，
    /// VM 内部状态保持一致，可继续跑其它脚本（§19）。
    static void* Allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize) noexcept;

    lua_State* state_{nullptr};
    LuaLimits limits_{};
    std::thread::id owner_thread_{};
    bool sandbox_open_{false};

    // 指令检查周期（hook 粒度）。
    std::uint32_t hook_period_{1000};

    // —— 计数 allocator 状态（allocator 由 Lua 从**同一线程**回调，热路径）——
    std::atomic<std::size_t> mem_used_{0};
    std::atomic<std::size_t> mem_peak_{0};
    std::atomic<std::size_t> alloc_calls_{0};
    std::atomic<std::size_t> alloc_fails_{0};

    // —— 限制与统计 ——
    std::atomic<std::size_t> limit_hits_{0};
    std::atomic<std::size_t> instr_checks_{0};
    std::atomic<std::uint32_t> version_{0};

    /// 当前执行预算（hook 读写；**只在所属线程**，无需原子）。
    /// 通过 `ExecScope` RAII 设置/清除，保证预算不会泄漏到下一次调用（§19）。
    struct ExecBudget {
        bool active{false};
        std::uint64_t instructions_left{0};
        core::SteadyNs deadline_ns{0};
        bool aborted{false};
        ScriptError reason{ScriptError::Ok};
    };
    ExecBudget budget_{};

    ErrorDetail last_error_{};

    // 供 src/ 内部使用：执行区间 RAII 与 hook 安装。
    // `ExecScope` 的**声明**在 public 区（同模块 src/ 需按值持有），这里只加友元。
    friend class ExecScope;
    static void InstallHook(lua_State* state, std::uint32_t period);
};

/// 执行区间 RAII（内部使用，定义在 src/lua_vm.cpp）。
///
/// 语义：进入时按当前 `LuaLimits` 装载指令 / 时间预算，退出时（含异常与 Lua error 的
/// longjmp 归位后）清空预算。**预算永不复用**，因此一次调用耗尽预算不会影响下一次调用
/// —— 这是 §19「限制触发后 VM 可继续跑其他脚本」的实现基础。
class LuaVM::ExecScope {
public:
    explicit ExecScope(LuaVM& vm) noexcept;
    ~ExecScope();
    ExecScope(const ExecScope&) = delete;
    ExecScope& operator=(const ExecScope&) = delete;

    /// 本区间是否因限额被中止（hook 置位）。
    ///
    /// 【踩坑】必须直接读**活跃预算**，而不是读析构时才填充的成员。
    /// 中止标志由 debug hook 写在 `LuaVM::budget_` 上，而 `budget_` 在本区间存活
    /// 期间就是活跃预算（析构时才换回外层预算）。调用方只能在
    /// `lua_pcall` 返回后、本区间**仍存活**时读取 —— 一旦本区间析构，对象本身就
    /// 不可访问了，析构里再赋值谁也用不到，所以那种写法恒返回 false。
    bool Aborted() const noexcept { return vm_.budget_.aborted; }
    ScriptError AbortReason() const noexcept { return vm_.budget_.reason; }

private:
    LuaVM& vm_;
    LuaVM::ExecBudget saved_{};
};

}  // namespace mmo::script
