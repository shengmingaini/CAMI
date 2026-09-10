#pragma once

/// TASK-032 · HotReloader —— Lua 热更新的六阶段流水线（§7 Public Interface 冻结契约）。
///
/// 架构位置
/// --------
///   - **状态归属**（§4）：当前生效脚本版本由 `ScriptContext` 拥有（SimulationThread）；
///     待激活的 `ReloadTicket` 由 `HotReloader` 拥有（Worker / 运维线程），
///     **激活瞬间原子交接**（一次 `ScriptContext` 调用，中间不存在「半个新版本」）。
///   - **线程模型**（§9）：`Prepare` / `Validate` 在 Worker 线程（不碰生产 VM，
///     另起**隔离临时 `ScriptContext`**，见 §15-3）；`Activate` / `Rollback` / `VerifyPass`
///     **只能在 SimulationThread 的 Tick Safe Point**。
///   - **热路径**（§10）：NO。Activate 位于 Tick 边界，必须 < 100us（§22）。
///
/// 六阶段流水线（§8，顺序不可变）
/// ------------------------------
/// ```
/// 状态机：Idle → Compiling → Validating → PendingActivate → Activated
///                                            ↘ RolledBack / Failed
///
/// 1 Load      读取新源码（任意线程）
/// 2 Compile   编译（任意线程，隔离临时 VM）
/// 3 Validate  语法 + 沙箱静态扫描 + 冒烟执行（任意线程，隔离临时 VM）
///   ─── 以上失败 = 旧版本完全不受影响（生产 VM 从未被触碰）───
/// 4 Activate  ★Tick Safe Point★ 原子替换模块表 / 函数引用（不重建 VM）
/// 5 Verify    下一安全点冒烟验证，异常则自动 Rollback
/// 6 Commit    记录 ScriptVersion + 审计日志（文件 IO 由 DrainAudit 在 Tick 外完成）
/// ```
///
/// 为什么 Activate 不在 Worker 线程编译
/// ------------------------------------
///   Lua 的编译产物（`lua_load` 的 chunk）与 registry 引用**绑定到具体的 `lua_State`**。
///   生产 VM 属 SimulationThread，Worker 线程的临时 VM 上编译出的 chunk 对它无效。
///   因此 `ReloadTicket` **只携带纯值**（源码 / checksum / 报告），Activate 时在生产 VM 上
///   重新编译一次。实测单脚本编译 ≈ 8us（TASK-031 `lua_load_ms_per_script=0.0080`），
///   远低于 §22 的 100us 停顿预算。
///
/// 安全点由**宿主**开启（§21 禁止 Tick 中途替换）
/// ---------------------------------------------
///   `Activate` / `Rollback` / `VerifyPass` 只有在 `InSafePoint() == true` 时才工作，
///   否则返回 `BUSY`。宿主在自己的 Tick 边界调用 `BeginSafePoint(tick)` /
///   `EndSafePoint()`。使用 TASK-013 `SimulationScheduler` 的宿主可直接注册
///   `hot_reload/script_reload_stage.h` 的 `ScriptReloadStage`（一行接入）。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/time/clock.h"

#include "mmo/script/script_context.h"

namespace mmo::script {

class IAuditSink;

/// §7 ReloadState —— 六阶段流水线的可观测状态（**每脚本一份**）。
enum class ReloadState : std::uint8_t {
    Idle = 0,       ///< 未发起过热更
    Compiling,      ///< 阶段 1-2：读取 + 编译中（Worker）
    Validating,     ///< 阶段 3：校验中（Worker）
    PendingActivate,///< 校验通过，等待安全点
    Activated,      ///< 阶段 4 已完成，等待阶段 5 验证
    RolledBack,     ///< 已回滚（人工或自动）
    Failed,         ///< 编译 / 校验 / 激活失败（旧版本仍在服务）
};

/// 名称化（日志 / 报告用）；未知值返回 "UNKNOWN"，禁止崩溃。
const char* ToString(ReloadState state) noexcept;

/// §7 ScriptVersion —— 一次**成功激活**的版本记录（审计与回滚的判据）。
struct ScriptVersion {
    ScriptId id{kInvalidScriptId};
    std::uint32_t version{0};
    /// 源码内容指纹。**非加密哈希**：本模块位于 `scripting/lua`，依赖集不含
    /// security/economy（§27.2），不得跨模块包含他方 `src/` 的 SHA-256 实现，
    /// 故用自包含的 FNV-1a 64（16 位十六进制）。用途是「同一版本去重 / 变更检测」，
    /// 不承担防篡改职责（防篡改需走 DataService 侧，见 docs/HOTRELOAD.md §7）。
    std::string checksum;
    std::int64_t activated_at_ms{0};
    std::string activated_by;
};

/// 校验报告（阶段 3 的产出）。
struct ValidationReport {
    bool ok{false};
    bool syntax_ok{false};   ///< 阶段 2 编译通过
    bool sandbox_ok{false};  ///< 禁用 API 静态扫描无命中
    bool smoke_ok{false};    ///< 隔离 VM 冒烟执行通过（未定义冒烟函数时视为通过）
    std::uint32_t smoke_calls{0};
    core::DurationMs elapsed{0};
    /// 问题清单（`ok == false` 时非空；每条形如 `sandbox: banned api 'io.open'`）。
    std::vector<std::string> issues;
};

/// §7 ReloadTicket —— 承载「已编译并校验过的待激活版本」。
///
/// 刻意只含**纯值**（见文件头「为什么 Activate 不在 Worker 线程编译」）：
/// 票证可安全跨线程传递，且未 Activate 前对运行中的 VM **零影响**（§15-2）。
struct ReloadTicket {
    std::uint64_t ticket_id{0};
    std::string name;
    std::string source;    ///< 待激活源码（Activate 时在生产 VM 上重新编译）
    std::string checksum;
    std::string origin;    ///< 来源描述（文件名 / 内联），用于审计
    ReloadState state{ReloadState::Idle};
    ValidationReport report{};
    /// Prepare 在**隔离 VM** 上完成的编译结果（仅用于尽早暴露语法错误）。
    bool compiled{false};
};

/// §7 HotReloader —— 热更新流水线。每 Scene 一个（与它服务的 `ScriptContext` 一一对应）。
///
/// 线程模型（§9，**硬约束**）：
///   | 方法 | 允许的线程 |
///   |---|---|
///   | `Prepare` / `PrepareFromFile` / `Validate` | 任意线程（通常是 Worker / 控制线程） |
///   | `Activate` / `Rollback` / `VerifyPass` | **仅 SimulationThread 且处于安全点** |
///   | `CurrentVersion` / `History` / `StateOf` / 各计数器 | 任意线程（加锁只读） |
///   | `DrainAudit` | **任意线程，但禁止在 Tick 内**（会做文件 IO） |
///
/// 生命周期：`ScriptContext` 必须长于本对象（构造时按引用持有）。
class HotReloader {
public:
    /// 【踩坑 · 必须用显式默认构造，不能用 NSDMI】
    ///   写成 `struct Config { bool x{true}; };`（成员默认初始化器 NSDMI）再配
    ///   `HotReloader(ScriptContext&, Config config = {})`，GCC 会报
    ///   `could not convert '<brace-enclosed initializer list>()' from ... to 'Config'`：
    ///   嵌套类 + NSDMI + 外层类自身声明里的默认实参，NSDMI 延迟解析来不及完成，
    ///   GCC 便认为 `Config` 无法由 `{}` 初始化（实测：非嵌套 OK、嵌套无 NSDMI OK、
    ///   嵌套有 NSDMI 必错）。仓库既有约定即 `SimulationScheduler::Config` 的写法
    ///   —— 显式默认构造 + 初始化列表。照抄该写法即可。
    struct Config {
        bool validate_before_activate;   ///< 未通过校验的票证禁止激活（§21）
        std::uint32_t max_rollback_versions;  ///< 版本历史上限（§20 验收 #5）
        core::DurationMs validate_budget;     ///< 校验总预算（§22：< 20ms/脚本 留余量）
        /// 冒烟函数名：脚本若定义该函数，`Validate`（隔离 VM）与 `VerifyPass`（生产 VM）
        /// 都会调用它；调用失败即判定脚本不可用 → 拦截 / 自动回滚。
        /// 未定义该函数时冒烟视为**通过**（不是所有脚本都需要自检入口）。
        std::string_view smoke_function;
        /// 允许 `PrepareFromFile` 读文件（§11 External IO：只在 Worker 线程发生）。
        bool allow_filesystem_read;

        Config()
            : validate_before_activate(true),
              max_rollback_versions(5),
              validate_budget(50),
              smoke_function("__hot_smoke"),
              allow_filesystem_read(true) {}
    };

    HotReloader(ScriptContext& context, Config config = {});

    /// 隔离 VM 的准备回调（加法扩展，**生产环境必须设置**）。
    ///
    /// 必要性：`Prepare` / `Validate` 在**隔离的临时 VM** 上编译与冒烟（§15-3），而真实脚本
    /// 会调用宿主绑定（`entity.*` / `skill.*` / `entity.set_hp` …）。若不把这些绑定同样装进
    /// 隔离 VM，冒烟执行必然因 `attempt to index a nil value` 失败 —— 结果是校验要么
    /// **全盘拦截合法脚本**，要么被 `validate_before_activate=false` 关掉而成摆设。
    /// 宿主在此回调里做与生产 VM 相同的装配（`AddBinding` / `BindCommandApi` / …）。
    /// 回调返回失败 → 本次 Prepare/Validate 判定为内部错误（不激活）。
    using IsolatedPreparer = core::Result<void> (*)(ScriptContext& ctx, void* user);
    void SetIsolatedPreparer(IsolatedPreparer fn, void* user) noexcept;

    ~HotReloader();
    HotReloader(const HotReloader&) = delete;
    HotReloader& operator=(const HotReloader&) = delete;
    HotReloader(HotReloader&&) = delete;
    HotReloader& operator=(HotReloader&&) = delete;

    // =======================================================================
    // §7 Public Interface（冻结契约）
    // =======================================================================

    /// 阶段 1-2：登记源码 + 编译（隔离临时 VM）。任意线程。
    /// 编译失败 → `INVALID_ARGUMENT`（脚本本身有问题，重试无意义），旧版本不受影响。
    core::Result<ReloadTicket> Prepare(std::string_view name, std::string_view source);

    /// 阶段 1-2 的文件变体：从磁盘读取（§11）。文件不存在 / 不可读 → `NOT_FOUND`（§19）。
    core::Result<ReloadTicket> PrepareFromFile(std::string_view name, std::string_view path);

    /// 阶段 3：沙箱静态扫描 + 冒烟执行（隔离临时 VM，不动生产 VM）。任意线程。
    /// 校验不通过时返回的 `ValidationReport::ok == false`（**不是** Result 错误：
    /// 「脚本不合格」是正常的业务结果，报告要交给调用方与审计，不是异常）。
    core::Result<ValidationReport> Validate(const ReloadTicket& ticket);

    /// 阶段 4：**只能在一个安全点内**原子替换。非安全点 / 非 OwnerThread → `BUSY`。
    /// 「未校验」→ `UNAUTHORIZED`（§21）。判据是**本对象内部**的已校验记录
    /// `(name, checksum)` 对齐，**不是**入参 `ticket.report.ok` —— 标准流程
    /// `Prepare → Validate → Activate`（原样传 `Prepare` 返回的那张票证）即可通过；
    /// 手工构造一张 `report.ok = true` 的假票证**不会**通过（见 .cpp 内注释）。
    core::Result<void> Activate(const ReloadTicket& ticket, core::TraceID trace);

    /// 回滚到该脚本的**上一个**版本（同样只能在安全点内）。无上一版本 → `NOT_FOUND`。
    core::Result<void> Rollback(std::string_view name, core::TraceID trace);

    /// 当前生效版本；该脚本从未成功激活过则返回 `nullptr`。
    const ScriptVersion* CurrentVersion(std::string_view name) const noexcept;

    /// 版本历史（最近 `max_rollback_versions` 条，下标 0 = 当前生效版本）。最多 5 条。
    std::vector<ScriptVersion> History(std::string_view name) const;

    // =======================================================================
    // 加法扩展（§7 之外的补充，不改变 §7 语义）
    // =======================================================================

    /// 某脚本的流水线状态（未记录的脚本返回 `Idle`）。
    ReloadState StateOf(std::string_view name) const noexcept;

    // ---- §13 Tick Safe Point 门（宿主驱动；TASK-013 宿主可用 ScriptReloadStage 一行接入）----

    /// 进入安全点（必须在 SimulationThread 上调用）。`tick` 为当前 Tick 号，供验证阶段判定
    /// 「切换到下一个 Tick 才算完成」。
    void BeginSafePoint(std::uint64_t tick) noexcept;
    /// 离开安全点。幂等。
    void EndSafePoint() noexcept;
    bool InSafePoint() const noexcept;
    /// 当前安全点所属 Tick 号；不在安全点时返回 0。
    std::uint64_t SafePointTick() const noexcept;

    /// 阶段 4 的第二半：把校验通过的**全部**待激活票证在本次安全点内激活。
    /// 返回实际激活的脚本数。典型用法是宿主在 Tick 边界调一次（见 ScriptReloadStage）。
    std::size_t ActivatePending(core::TraceID trace);

    /// 阶段 5：对上一安全点激活的脚本做冒烟验证；失败者**自动回滚**。
    /// 必须在安全点内调用（需要改生产 VM）。返回本次处理的脚本数。
    std::size_t VerifyPass(core::TraceID trace);

    // ---- 阶段 6 审计 ----

    /// 安装审计 sink（不接管所有权）。默认无 sink（仅内存记录）。
    void SetAuditSink(IAuditSink* sink) noexcept;

    /// 把已激活版本交给 sink。**会做文件 IO，禁止在 Tick 内调用**（§11）。
    /// 返回本次真正写出的记录数；无 sink 时返回 0（记录保留在队列中）。
    std::size_t DrainAudit();

    // ---- 观测 ----

    std::uint64_t ActivatedCount() const noexcept;
    std::uint64_t AutoRollbackCount() const noexcept;
    std::uint64_t FailedCount() const noexcept;
    std::size_t ScriptCount() const noexcept;
    /// 待激活（已过校验，等安全点）的票证数。
    std::size_t PendingCount() const noexcept;
    const Config& Cfg() const noexcept { return config_; }

private:
    /// 每脚本一份的可变状态（`mu_` 保护；`history` 内含源码，用于回滚）。
    struct Entry {
        ReloadState state{ReloadState::Idle};
        /// 生产 VM 上的脚本句柄（`Load` / `ReloadInPlace` 返回）。
        /// 必须记住：`ScriptContext` 只提供 `NameOf(id)`，**没有** `IdOf(name)`，
        /// 而冒烟验证要按 id 调 `Call`，所以句柄只能由本对象在替换时捕获。
        ScriptId current_id{kInvalidScriptId};
        std::uint32_t next_version{1};
        ScriptVersion current{};
        /// 历史（下标 0 = 当前）。元素含源码，故上限严格受 `max_rollback_versions` 约束，
        /// 否则连续热更会无限吃内存（§21「禁止无限保留历史版本」/ §19「连续热更不泄漏」）。
        struct Past {
            ScriptVersion version;
            std::string source;
            std::string origin;
        };
        std::vector<Past> history;
        ReloadTicket pending{};
        bool has_pending{false};
        /// 已激活但尚未通过阶段 5 验证。
        bool needs_verify{false};
        std::uint64_t activated_tick{0};
    };

    Entry* FindEntry(std::string_view name);
    const Entry* FindEntry(std::string_view name) const;
    /// 建一个与生产 VM 同装配的隔离临时 `ScriptContext`（调用方负责让它析构）。
    core::Result<std::unique_ptr<ScriptContext>> CreateIsolated() const;
    /// 在生产 VM 上执行一次「替换模块表 / 函数引用」；已装载 → ReloadInPlace，否则 → Load。
    /// 返回生产 VM 上该脚本的句柄。
    core::Result<ScriptId> SwapIn(const std::string& name, const std::string& source);
    /// 冒烟调用：调用 `id` 上的 `config_.smoke_function`。未定义该函数 → 视为通过（true）。
    bool SmokeCall(ScriptContext& target, ScriptId id) const;
    core::Result<void> ActivateLocked(Entry& entry, const ReloadTicket& ticket, core::TraceID trace,
                                      bool is_rollback, std::string_view actor);
    void RecordAudit(const ScriptVersion& version, std::string_view name);

    ScriptContext& context_;
    Config config_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>> entries_;

    std::vector<std::pair<ScriptVersion, std::string>> audit_queue_;
    IAuditSink* sink_{nullptr};
    IsolatedPreparer isolated_preparer_{nullptr};
    void* isolated_user_{nullptr};

    std::uint64_t next_ticket_id_{1};
    std::atomic<std::uint64_t> activated_count_{0};
    std::atomic<std::uint64_t> auto_rollback_count_{0};
    std::atomic<std::uint64_t> failed_count_{0};
    std::atomic<std::uint64_t> safe_point_tick_{0};
    std::atomic<bool> in_safe_point_{false};
};

}  // namespace mmo::script
