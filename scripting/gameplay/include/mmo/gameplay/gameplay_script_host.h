#pragma once

/// TASK-033 · GameplayScriptHost —— 「系统钩子 ↔ 玩法脚本」的桥接层（§7 / §15.2）。
///
/// 为什么需要这一层（本任务的核心交付）
/// ------------------------------------
/// TASK-018/019/021 只导出**纯 C++ 接口**（`SkillSystem::TryCast` / `QuestSystem::OnEvent` /
/// `AiSystem::Update`），没有任何 Lua 钩子位。TASK-031/032 提供了 Lua 运行时与热更流水线，
/// 但也没有「业务系统怎么调脚本」的答案。**本模块就是这两者之间缺失的那一段**：
///
/// ```
///   业务系统（C++，状态 Owner）
///        │  ① 提供纯值请求（属性 / HP% / 事件字段）
///        ▼
///   GameplayScriptHost ──② 按 config/gameplay/scripts.json 路由到脚本
///        │  ③ Call(id, "on_event", ctx, name)      ← 只传标量（TASK-031 冻结签名）
///        │     脚本内 payload 由 gameplay.payload() 绑定提供（表通道）
///        │     脚本结果由 gameplay.result{…} 绑定回传（TASK-031 INTERFACE §6 通路 2）
///        ▼
///   Lua 脚本（四入口：on_init / on_event / on_tick / on_reload）
///        │  ④ 受控写：quest.set_progress / skill.cast / entity.set_hp → ScriptCommand
///        ▼
///   宿主注册的 op handler（业务系统）→ 状态真正被改写
/// ```
///
/// 五条不可越过的红线（§7 / §21）
/// ------------------------------
///   1. **脚本从不直接改实时状态**：所有写都是一条 `ScriptCommand`，由状态 Owner 执行；
///   2. **位置积分 / AOI / 伤害最终结算不下放 Lua**：本层只回传「数值」，
///      结算顺序、随机（暴击 / 闪避）、护盾吸收仍由 TASK-022 的 C++ 结算链完成；
///   3. **脚本返回值一律经 C++ 校验并钳制**（NaN / 负数 / 超上限 → 钳 + 标记 `clamped`）；
///   4. **禁止脚本做 IO**：由 TASK-031 白名单沙箱在结构上保证（本层不提供任何 IO 绑定）；
///   5. **周期脚本 ≤ 1Hz**：清单层直接拒绝 `tick_hz > 1`（配置错误在装载期暴露）。
///
/// 线程模型（§9）
/// -------------
///   - 一个 `GameplayScriptHost` 拥有一个 `ScriptContext`（= 每 Scene 一个 VM）；
///   - **所有公开方法都必须在创建线程（SimulationThread / OwnerThread）调用**；
///     跨线程会经由 `ScriptContext` 返回 `BUSY`（不崩溃、不静默）。
///   - 热更的 `Prepare`/`Validate` 走 TASK-032 的隔离临时 VM（不碰生产 VM）；
///     `Activate` 只在宿主开启的 Tick 安全点内发生。
///
/// 失败隔离（§19）
/// --------------
///   - 单个脚本语法错 / 运行期报错 / 超限 → **只作废该次调用**，其余脚本与 Scene Tick 照常；
///   - 装载期失败默认 fail-fast（`Config::require_all_scripts`），关闭后则记录并继续，
///     此时对应 hook **回退到 C++ 默认行为**（路由表里没有它 = 没有任何脚本处理）。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/command_bus.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/game/entity/entity_manager.h"

#include "mmo/gameplay/gameplay_manifest.h"
#include "mmo/gameplay/gameplay_types.h"

#include "mmo/script/hot_reload/audit_sink.h"
#include "mmo/script/hot_reload/hot_reloader.h"
#include "mmo/script/script_context.h"

namespace mmo::gameplay {

/// 脚本受控写的落地通道：宿主把状态 Owner 的 op handler 装进来。
/// 签名与 TASK-031 `ScriptCommandHandler` 一致（fn + user：注册期一次、调用期零分配）。
using GameplayCommandHandler = core::Result<script::ScriptReply> (*)(
    const script::ScriptCommand& cmd, void* user);

class GameplayScriptHost {
public:
    /// 【踩坑】必须显式默认构造 + 初始化列表，**不能用 NSDMI**：
    /// 嵌套 `Config` 带成员默认初始化器会让 GCC 在「外层默认实参」场景报
    /// `could not convert '<brace-enclosed initializer list>()' ... to 'Config'`
    /// （TASK-032 已实测，仓库约定照抄 `SimulationScheduler::Config`）。
    struct Config {
        std::size_t memory_bytes;
        std::uint32_t max_instructions;
        core::DurationMs max_exec_time;
        std::uint32_t max_stack_depth;
        std::uint32_t max_scripts;
        std::uint32_t max_tick_hz;
        std::uint32_t scene_id;
        std::string root;            ///< 仓库根（拼相对路径；空 = 直接用清单里的 path）
        bool require_all_scripts;    ///< 装载期有脚本失败即整体失败（启动期 fail-fast）
        bool allow_filesystem_read;  ///< 清单 / 脚本文件读取（Cold Path；§11）

        Config()
            : memory_bytes(16u * 1024u * 1024u),
              max_instructions(2'000'000u),
              max_exec_time(5),
              max_stack_depth(64u),
              max_scripts(64u),
              max_tick_hz(1u),
              scene_id(0u),
              root(),
              require_all_scripts(true),
              allow_filesystem_read(true) {}
    };

    static core::Result<std::unique_ptr<GameplayScriptHost>> Create(const Config& config);

    ~GameplayScriptHost();
    GameplayScriptHost(const GameplayScriptHost&) = delete;
    GameplayScriptHost& operator=(const GameplayScriptHost&) = delete;
    GameplayScriptHost(GameplayScriptHost&&) = delete;
    GameplayScriptHost& operator=(GameplayScriptHost&&) = delete;

    // =======================================================================
    // 装配（启动期，Cold Path）
    // =======================================================================

    /// 注入上游系统接口（与 TASK-031 `Bind*Api` 同口径；未绑定时相关绑定返回明确错误）。
    core::Result<void> BindEntityManager(game::EntityManager& entities);
    core::Result<void> BindEventBus(core::EventBus& events);
    core::Result<void> BindCommandBus(core::CommandBus& commands);

    /// 注入 `quest.set_progress` / `skill.cast` / `entity.set_hp` 等 op 的执行者（状态 Owner）。
    void BindCommandHandler(GameplayCommandHandler handler, void* user) noexcept;

    /// 注入外部引用注册表（§19「脚本引用的 Quest/Boss 不存在 → 加载期校验报错」）。
    /// 未注入时 `requires` 校验跳过（在 `LoadErrors()` 里显式记录「未校验」，不静默放行）。
    void SetReferenceRegistry(const IReferenceRegistry* registry) noexcept;

    /// 审计 sink（TASK-032 阶段 6）。未设置时审计只留在内存队列。
    void SetAuditSink(script::IAuditSink* sink) noexcept;

    /// 按清单装载（编译 → 校验 → 激活 → 契约探针 → `on_init`）。返回**成功装载**的脚本数。
    core::Result<std::size_t> LoadManifest(const ScriptManifest& manifest);
    core::Result<std::size_t> LoadManifestFromFile(std::string_view path);

    // =======================================================================
    // 五个系统钩子（§15.3–§15.7）
    // =======================================================================

    /// ① 技能公式：C++ 传属性 → Lua 算数值 → **本层钳制**后交回 C++ 结算（§22 < 3us）。
    /// 无脚本认领该 `skill_id` 时返回 `matched == false`（调用方走 C++ 默认公式）。
    SkillFormulaResult ComputeSkillFormula(const SkillFormulaRequest& request);

    /// ② 任务事件：C++ 发 `MonsterKilled` 等 → Lua 更新进度（经 `quest.set_progress` 命令）。
    /// 返回被调用的脚本数（0 = 无脚本认领，C++ 走默认规则）。
    std::size_t DispatchQuestEvent(const GameplayPayload& event);

    /// ③ NPC 决策：Lua 只选动作，C++ 状态机执行（§15.5）。
    AiDecision DecideAi(const AiContext& context);

    /// ④ Boss 阶段：按 HP% 切阶段 / 换技能组 / 召唤 / 要求广播（§15.6）。
    BossPhaseDecision CheckBossPhase(const BossContext& context);

    /// ⑤ 活动修正：限时倍率（含「活动结束自动失效」判定；时间由 C++ 注入 → §25 确定性）。
    ActivityModifier QueryActivity(const ActivityQuery& query);

    /// 周期脚本（§9：默认 1Hz）。`dt_seconds` 由宿主 Tick 提供。
    /// 只有清单里 `tick_hz > 0` 的脚本会被调用，且**最多每 1 秒一次**（累加器判定）。
    /// 返回本次真正触发 `on_tick` 的脚本数。
    std::size_t Tick(double dt_seconds);

    // =======================================================================
    // 热更（§15.9 实战演练；流水线本身归 TASK-032）
    // =======================================================================

    /// 进入 Tick 安全点（由宿主在 Tick 边界调用）。注意：`ScriptReloadStage` 属于
    /// TASK-032（scripting/lua），并非 TASK-013 产出；TASK-013 的 SimulationScheduler
    /// 当前未提供安全点钩子，生产环境的安全点接线为已知架构缺口（见 docs/gameplay-script-report.md §7）。
    void BeginSafePoint(std::uint64_t tick) noexcept;
    void EndSafePoint() noexcept;
    bool InSafePoint() const noexcept;

    /// 阶段 1–3（任意线程）：登记源码 + 隔离 VM 编译 + 校验。
    core::Result<script::ReloadTicket> PrepareReload(std::string_view name, std::string_view source);
    core::Result<script::ReloadTicket> PrepareReloadFromFile(std::string_view name,
                                                            std::string_view path);

    /// 阶段 3 的收口（任意线程）：沙箱静态扫描（禁用 API）+ 隔离 VM 冒烟执行。
    ///
    /// 【为什么必须是独立一步】TASK-032 §21「禁止未校验直接激活」的判据是流水线**内部**
    /// 的 `(name, checksum)` 已校验记录，不是入参的 `report.ok`。因此
    /// `PrepareReload → ActivateReload` 这种省略写法会**必然**返回 `UNAUTHORIZED`
    /// （本方法缺失时，热更路径事实上等于没有校验阶段 —— 单元测试已固化「标准三步」）。
    /// 标准顺序（阶段 4/5/6 之外的其余阶段）：
    /// ```
    ///   PrepareReload（Worker） → ValidateReload（Worker） → ActivateReload（★安全点★）
    /// ```
    core::Result<script::ValidationReport> ValidateReload(const script::ReloadTicket& ticket);

    /// 阶段 4（**仅安全点内**）：原子替换。脚本名必须在清单里（否则 `NOT_FOUND`）。
    /// 前置条件：该票证已过 `ValidateReload`（流水线内部以 `(name, checksum)` 对齐），
    /// 否则返回 `UNAUTHORIZED`。激活后增量执行 §7 契约校验 + `on_reload(old_version)`；
    /// 契约破损就地回滚，不把缺入口的版本留在线上。
    core::Result<void> ActivateReload(const script::ReloadTicket& ticket, core::TraceID trace);

    /// 回滚到上一个版本（**仅安全点内**）。
    core::Result<void> Rollback(std::string_view name, core::TraceID trace);

    /// 阶段 5：对上一安全点激活的脚本冒烟验证，失败自动回滚。
    std::size_t VerifyPass(core::TraceID trace);

    /// 阶段 6：把审计队列交给 sink 落盘（**禁止在 Tick 内**，会做文件 IO，§11）。
    std::size_t DrainAudit();

    /// 当前生效版本；从未激活过返回 nullptr。
    const script::ScriptVersion* CurrentVersion(std::string_view name) const noexcept;

    // =======================================================================
    // 观测 / 运维
    // =======================================================================

    const LoadStats& Stats() const noexcept { return stats_; }
    const ScriptManifest& Manifest() const noexcept { return manifest_; }
    /// 装载期失败 / 提示原因（每条形如 `<script>: <reason>`）。空 = 全部成功。
    const std::vector<std::string>& LoadErrors() const noexcept { return load_errors_; }
    /// 脚本源码路径（数据来自清单 —— **本模块没有任何脚本名字面量**）。
    std::string_view PathOf(std::string_view name) const noexcept;
    /// 脚本逻辑名列表（清单顺序；运维报表用）。
    std::vector<std::string> ScriptNames() const;

    /// 某 hook 下注册的路由数。
    std::size_t RouteCount(Hook hook) const noexcept;

    std::uint64_t TickNumber() const noexcept { return tick_; }
    /// 「运行期脚本错误」累计次数（单脚本报错被隔离的计数，§19）。
    std::uint64_t ScriptErrorCount() const noexcept { return script_errors_; }
    /// 各 hook 的调用累计次数（观测 / bench 用）。
    std::uint64_t HookCallCount(Hook hook) const noexcept {
        return hook_calls_[static_cast<std::size_t>(hook)];
    }

    script::ScriptContext& Context() noexcept { return *context_; }
    script::HotReloader& Reloader() noexcept { return *reloader_; }

private:
    explicit GameplayScriptHost(const Config& config);

    /// 已装载脚本的宿主侧记录。
    struct Entry {
        ScriptBinding binding;
        script::ScriptId id{script::kInvalidScriptId};
        std::uint32_t version{1};
        bool has_tick{false};
        /// 周期累加器（毫秒）；只有 `tick_hz > 0` 有意义。
        double tick_accum_ms{0.0};
    };

    /// 路由：`key → 若干 entry 下标`（清单顺序；短路语义 = 首个认领者生效）。
    using RouteList = std::vector<std::size_t>;
    using RouteMap = std::unordered_map<std::string_view, RouteList>;

    /// 一条脚本的装载结果。
    struct Loaded {
        script::ScriptId id{script::kInvalidScriptId};
        std::uint32_t version{1};
    };

    core::Result<Loaded> LoadOne(const ScriptBinding& binding);
    /// 校验 §7 四入口齐全（装载期契约探针）：`on_init` **真实调用**，
    /// `on_event` 收 `__contract_probe`、`on_tick` 收 dt=0、`on_reload` 收 old_version=0。
    /// 脚本必须在这三个入参下**无副作用**（docs/README.md 约定 R1）。
    core::Result<void> CheckEntryContract(const ScriptBinding& binding, script::ScriptId id);
    /// 热更后的契约校验：探针 `on_event`/`on_tick` + **真实** `on_reload(old_version)`。
    /// 刻意不重跑 `on_init`：`ReloadInPlace` 保留脚本全局状态，重跑初始化会破坏状态连续性
    /// （TASK-032 §17「切换后状态连续」）。
    core::Result<void> CheckReloadContract(const Entry& entry, std::int64_t old_version);

    /// 调用一个入口并区分「函数缺失」与「函数报错」，产出可读的失败原因。
    /// `mode`：0 = 只传 ctx 句柄；1 = 追加 double；2 = 追加 integer。
    core::Result<void> RequireEntry(script::ScriptId id, std::string_view fn,
                                    std::string_view script_name, int mode, double num_arg,
                                    std::int64_t int_arg);

    Entry* FindEntry(std::string_view name) noexcept;
    const Entry* FindEntry(std::string_view name) const noexcept;
    /// 清单里的相对路径 → 实际路径（`Config::root` 非空时前置）。
    std::string ResolvePath(std::string_view path) const;
    /// 把一次失败的 `Call` 渲染成 `消息 (脚本行)` 形式。
    std::string DescribeCallFailure(const core::Result<void>& result) const;

    void CollectRoutes(Hook hook, std::string_view key) const;
    core::Result<void> InvokeEvent(const Entry& entry, Hook hook);
    core::Result<void> InvokeStr(const Entry& entry, std::string_view fn);
    core::Result<void> InvokeNum(const Entry& entry, std::string_view fn, double v);
    core::Result<void> InvokeInt(const Entry& entry, std::string_view fn, std::int64_t v);

    core::Result<void> InstallOps();
    core::Result<void> InstallGameplayBindings();
    /// 隔离 VM 的装配回调（TASK-032 §15-3：隔离 VM 必须与生产 VM 同装配，
    /// 否则合法脚本会因 `attempt to index a nil value` 被全盘误拦）。
    static core::Result<void> PrepareIsolated(script::ScriptContext& ctx, void* user);

    /// 契约探针用的合成事件名。
    static constexpr std::string_view kContractProbe = "__contract_probe";

    /// 命令 op 的落地蹦床：所有 op 共用（op 名在 `cmd.Op()` 里）。
    static core::Result<script::ScriptReply> CommandTrampoline(const script::ScriptCommand& cmd,
                                                               void* user);

    // ---- `gameplay.*` 原生绑定（静态成员函数：满足 NativeFn 的裸函数指针签名）----
    static core::Result<int> BindingCtx(script::ScriptCall& call, void* user);
    static core::Result<int> BindingPayload(script::ScriptCall& call, void* user);
    static core::Result<int> BindingResult(script::ScriptCall& call, void* user);
    static core::Result<int> BindingHook(script::ScriptCall& call, void* user);
    static void FillCtx(script::TableWriter& out, void* user);
    static void FillPayload(script::TableWriter& out, void* user);

    /// 绑定取回宿主实例（`user` 指针的唯一合法解释）。
    static GameplayScriptHost* Self(void* user) noexcept {
        return static_cast<GameplayScriptHost*>(user);
    }

    Config config_;
    std::unique_ptr<script::ScriptContext> context_;
    std::unique_ptr<script::HotReloader> reloader_;
    game::EntityManager* entities_{nullptr};

    ScriptManifest manifest_;
    std::vector<Entry> entries_;
    RouteMap routes_;
    mutable std::vector<std::size_t> scratch_;  ///< `CollectRoutes` 输出（复用，避免热路径分配）

    /// 本次调用的宿主侧状态（绑定读取；同步调用，OwnerThread 独占）。
    GameplayPayload payload_{};
    struct ResultSlot {
        bool has{false};
        std::int64_t i0{0};
        std::int64_t i1{0};
        std::int64_t i2{0};
        double d0{0.0};
        double d1{0.0};
        bool b0{false};
        bool b1{false};
        void Reset() noexcept {
            has = false;
            i0 = 0;
            i1 = 0;
            i2 = 0;
            d0 = 0.0;
            d1 = 0.0;
            b0 = false;
            b1 = false;
        }
    };
    ResultSlot slot_{};
    const Entry* current_{nullptr};

    /// `gameplay.ctx()` 的字段快照（`SetResultTable` 的 fill 期间有效）。
    struct CtxView {
        std::int64_t scene{0};
        std::int64_t tick{0};
        std::int64_t version{1};
        std::string_view script{};
        std::string_view category{};
        std::string_view hook{};
    };
    CtxView ctx_view_{};
    std::string handle_;

    GameplayCommandHandler handler_{nullptr};
    void* handler_user_{nullptr};
    const IReferenceRegistry* references_{nullptr};

    LoadStats stats_{};
    std::vector<std::string> load_errors_;
    std::uint64_t tick_{0};
    std::uint64_t script_errors_{0};
    std::uint64_t hook_calls_[5]{};
};

}  // namespace mmo::gameplay
