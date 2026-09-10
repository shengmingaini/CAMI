#pragma once

/// TASK-031 · ScriptContext —— 每 Scene 一个 Lua 运行时门面（§7 Public Interface 冻结契约）。
///
/// 架构位置（§4 State Owner / §9 Thread Model）
/// ------------------------------------------
///   - **每 Scene 一个实例，禁止全局单例**（§21）：`Create` 是工厂，实例由 Scene 的
///     SimulationThread 持有；创建线程即 OwnerThread，跨线程调用一律 `BUSY`。
///   - 脚本自身状态（全局变量）由本对象**间接拥有**：每个已加载脚本拿到一张**独立 `_ENV` 表**
///     （元表 `__index = _G`），因此脚本 A 的全局写不会污染脚本 B，也不会污染沙箱全局。
///     这满足 §4「同一实时状态只有一个权威写入者」——脚本从不直写 C++ 内存。
///   - 脚本状态归属 Scene 的 SimulationThread；脚本**不得**持有 C++ 对象指针跨 Tick，
///     一律用 `EntityId` 句柄（§4 / §21）。
///
/// 五类绑定（§8 / §20.5）
/// ---------------------
/// | 脚本 API | 落地方式 | 说明 |
/// |---|---|---|
/// | `entity.get/get_pos/set_hp` | 直连 `game::EntityManager` + `CommandBus` | 只读直查；`set_hp` 是**受控写**，转发命令给状态 Owner |
/// | `skill.cast` | `CommandBus`（`ScriptCommand{op="skill.cast"}`） | 由 SkillSystem 注册的 op handler 执行，脚本改不了伤害数值 |
/// | `quest.set_progress/complete` | `CommandBus` | 同上，由 QuestSystem 执行 |
/// | `event.subscribe/publish` | **真实** `core::EventBus` | 异步；实体生命周期事件用 TASK-011 的具体事件类型 |
/// | `query.ask` | **真实** `core::QueryBus` | 只读；运行期禁止副作用 |
///
/// Tick Safe Point（§13）
/// ---------------------
///   - `Load` / `Unload` 在**没有脚本正在执行**时立即生效；
///   - 若在脚本执行中（`InScriptExecution() == true`，例如 Lua 回调里又触发热更）被调用，
///     则**暂存**到 pending 队列，由宿主在 Tick 安全点调用 `ApplyPendingChanges()` 统一切换
///     —— 绝不在正在执行的 Tick 中途替换脚本；
///   - 若该次执行因限额被中止（MemoryLimit / Timeout …），暂存的变更**回滚丢弃**（§15.4「回滚到安全点」）。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/command_bus.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/bus/query_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"

#include "mmo/game/entity/entity_manager.h"

#include "mmo/script/lua_vm.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_event.h"
#include "mmo/script/script_value.h"

namespace mmo::script {

/// §7 ScriptId —— 已加载脚本的句柄（0 保留为非法值）。
using ScriptId = std::uint32_t;
inline constexpr ScriptId kInvalidScriptId = 0;

// ---------------------------------------------------------------------------
// Call 参数的编解码（`Call` 是 §7 的模板接口，编解码需在头内完成）
// ---------------------------------------------------------------------------

inline ScriptValue MakeScriptValue(std::nullptr_t) noexcept { return ScriptValue::Nil(); }
inline ScriptValue MakeScriptValue(bool v) noexcept { return ScriptValue::Bool(v); }
inline ScriptValue MakeScriptValue(ScriptValue v) noexcept { return v; }
inline ScriptValue MakeScriptValue(std::string_view v) noexcept { return ScriptValue::Str(v); }
inline ScriptValue MakeScriptValue(const std::string& v) noexcept { return ScriptValue::Str(v); }
inline ScriptValue MakeScriptValue(const char* v) noexcept {
    return ScriptValue::Str(v != nullptr ? std::string_view(v) : std::string_view{});
}
inline ScriptValue MakeScriptValue(char* v) noexcept {
    return ScriptValue::Str(v != nullptr ? std::string_view(v) : std::string_view{});
}

template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>)
inline ScriptValue MakeScriptValue(T v) noexcept {
    return ScriptValue::Int(static_cast<std::int64_t>(v));
}

template <typename T>
    requires std::floating_point<T>
inline ScriptValue MakeScriptValue(T v) noexcept {
    return ScriptValue::Num(static_cast<double>(v));
}

/// 脚本运行时门面。
///
/// Thread Ownership：**创建线程独占**。所有公开方法都要求调用者位于 `OwnerThread`，
/// 否则返回 `BUSY`（noexcept 的统计接口除外，它们可跨线程只读）。
/// Lifetime：由 Scene 拥有，长于全部已加载脚本。
/// Hot Path：`Call` / `DispatchEventToSubscribers` 是热路径；`Load` / `Unload` /
/// `ApplyPendingChanges` 是 Cold Path（启动期与热更安全点）。
class ScriptContext {
public:
    // =======================================================================
    // §7 Public Interface（冻结契约，禁止破坏性变更）
    // =======================================================================

    /// §7 工厂：创建 VM + 打开白名单沙箱 + 注册内置绑定面。
    /// 失败返回 INTERNAL_ERROR（绝不抛异常）。
    static core::Result<std::unique_ptr<ScriptContext>> Create(LuaLimits limits);

    ~ScriptContext();
    ScriptContext(const ScriptContext&) = delete;
    ScriptContext& operator=(const ScriptContext&) = delete;
    ScriptContext(ScriptContext&&) = delete;
    ScriptContext& operator=(ScriptContext&&) = delete;

    /// §7 加载脚本：编译 + 校验 + （无脚本在执行时）立即激活。
    ///
    /// 约定：chunk 在**独立 `_ENV`** 中执行；若 chunk 返回 table，则该表成为脚本的
    /// **模块表**（`_ENV` 作为其 `__index` 兜底），否则以 `_ENV` 表为模块表。
    /// `Call(id, fn)` 在模块表中查 `fn`。
    ///
    /// 失败：语法错误 → CompileError；chunk 顶层执行出错 → 对应 ScriptError。
    /// **失败时不留任何半成品**（脚本不会被登记，版本号不变）。
    core::Result<ScriptId> Load(std::string_view name, std::string_view source);

    /// §7 卸载脚本：释放其模块表与 `_ENV`（脚本全局状态随之回收）。
    /// 幂等：未加载的 id 返回 `NOT_FOUND`（与 TASK-011 Destroy 的幂等口径一致，可安全重试）。
    core::Result<void> Unload(ScriptId id);

    /// §7 调用脚本函数。参数支持：整型 / 浮点 / bool / `const char*` / `string_view` /
    /// `std::string` / `ScriptValue` / `nullptr`（≤ `ScriptArgs::kCapacity` 个）。
    ///
    /// 返回值：本任务只做「执行成功/失败」判定（签名冻结为 `Result<void>`）；
    /// 脚本返回值如需回传，走 `event.*` 或表参数的输出字段（见 docs/INTERFACE.md §6）。
    template <typename... Args>
    core::Result<void> Call(ScriptId id, std::string_view fn, Args&&... args) {
        ScriptArgs boxed;
        bool ok = true;
        ((ok = ok && boxed.Push(MakeScriptValue(std::forward<Args>(args)))), ...);
        if (!ok) {
            return core::Result<void>::Fail(core::Error(
                core::ErrorCode::INVALID_ARGUMENT, "too many script args",
                core::domain::kLua));
        }
        return CallWithArgs(id, fn, boxed);
    }

    /// §7 绑定 Entity 能力（只读直查 + `set_hp` 受控写）。
    core::Result<void> BindEntityApi(game::EntityManager& entities);

    /// §7 绑定 Event 能力（真实 EventBus，异步）。
    core::Result<void> BindEventApi(core::EventBus& events);

    /// §7 绑定 Query 能力（真实 QueryBus，只读）。
    core::Result<void> BindQueryApi(core::QueryBus& queries);

    /// §7 内存占用（计数 allocator 实时值；noexcept + 原子读，可热路径采集）。
    std::size_t MemoryUsed() const noexcept;

    /// §7 脚本版本（§13：脚本集合每次实际切换后 +1）。
    std::uint32_t Version() const noexcept;

    // =======================================================================
    // 扩展（additive：§7 之外的补充，不改变 §7 语义）
    // =======================================================================

    /// §8 `skill.*` / `quest.*` 的落地通道：绑定统一 `CommandBus`。
    ///
    /// 本模块**不依赖** combat / quest 模块（§27.2 依赖集不含 021/019），因此脚本的
    /// `skill.cast` / `quest.*` 被表达为 `ScriptCommand{op=…}` 交给总线，由业务系统注册的
    /// op handler 执行（见 `RegisterCommandOp`）。这既是「只走系统接口」（§20.5），
    /// 也避免为脚本再开一条 RPC 通道（§4）。
    core::Result<void> BindCommandApi(core::CommandBus& commands);

    // ---- 脚本信息 ----

    bool IsLoaded(ScriptId id) const noexcept;
    std::string_view NameOf(ScriptId id) const noexcept;

    /// **当前已装载**的脚本数量（定义见 src/script_context.cpp）。
    ///
    /// 不是 `scripts_.size()`：`scripts_` 是「脚本槽位表」，卸载只把槽位的
    /// `module_ref` 置 -1（槽位保留，避免 id 复用导致旧 id 复活成另一个脚本），
    /// 所以槽位数只增不减。判定「已装载」必须与 `IsLoaded` 同口径
    /// （`module_ref >= 0`），否则卸载后计数不退，调用方的装载数断言永远不成立。
    std::size_t LoadedCount() const noexcept;

    // ---- §13 Tick Safe Point ----

    /// 待生效的脚本变更数（热更暂存队列长度）。
    std::size_t PendingCount() const noexcept { return pending_.size(); }

    /// 当前是否正在执行脚本（Lua 调用栈上有本 VM 的脚本帧）。热更/卸载的时间判据。
    bool InScriptExecution() const noexcept { return run_depth_ > 0; }

    /// 在 Tick 安全点统一应用暂存的脚本变更（§13 Activate 阶段）。
    /// 幂等：无待生效变更时返回 OK。正在执行脚本时返回 `BUSY`（禁止 Tick 中途热切）。
    core::Result<void> ApplyPendingChanges();

    // ---- §27.4 扩展点（注册表机制：新增能力只 Add，不在 switch 里穷举）----

    /// 注册一条脚本绑定。重名 / 空函数 → `INVALID_ARGUMENT`；注册表冻结后 → `BUSY`。
    core::Result<void> AddBinding(BindingDef def);

    /// 冻结绑定表（装配期结束）。冻结后不再允许 Add，保证 Tick 期绑定稳定。
    void FreezeBindings() noexcept { bindings_.Freeze(); }
    bool BindingsFrozen() const noexcept { return bindings_.Frozen(); }
    const BindingRegistry& Bindings() const noexcept { return bindings_; }

    /// 注册 `skill.cast` / `quest.*` / `entity.set_hp` 等**命令** op 的实现（由业务系统提供）。
    core::Result<void> RegisterCommandOp(std::string_view op, ScriptCommandHandler handler,
                                         void* user);
    /// 注册 `query.ask` 的只读 op 实现（由业务系统提供）。
    core::Result<void> RegisterQueryOp(std::string_view op, ScriptQueryHandler handler, void* user);

    /// 事件桥安装器：把一个脚本事件名绑定到**具体 C++ 事件类型**的订阅与编解码。
    ///
    /// `install` 负责在真实 `core::EventBus` 上订阅目标事件类型；其回调里应把事件字段
    /// 填成表并转调 `DispatchEventToSubscribers(name, fill, ev)`。
    /// 返回该订阅 ID（ScriptContext 析构时会自动退订，避免 lambda 捕获的 `this` 悬垂）。
    using EventBridgeInstall = core::Result<core::EventBus::SubId> (*)(
        core::EventBus& bus, ScriptContext& ctx, std::string_view name, void* user);

    /// 注册一条事件桥（宿主扩展点）。同名重复注册返回 `INVALID_ARGUMENT`。
    core::Result<void> RegisterEventBridge(std::string_view name, EventBridgeInstall install,
                                           void* user);

    /// 已注册的 op 数（命令 / 查询）。
    std::size_t CommandOpCount() const noexcept { return command_ops_.size(); }
    std::size_t QueryOpCount() const noexcept { return query_ops_.size(); }

    /// 注入的上游系统接口（只读）。
    const ScriptServices& Services() const noexcept { return services_; }

    // ---- 事件派发（供 `event.*` 绑定实现与宿主订阅回调使用）----

    /// 把一次事件派发给订阅了 `name` 的全部脚本回调。
    ///
    /// `fill` 负责用 `TableWriter` 把事件字段写进传给脚本的 Lua 表；`ev` 原样透传给 `fill`
    /// （对 `ScriptEvent` 传 `nullptr`，表示「表已由本函数按 name 直接构造」）。
    /// 返回成功派发的订阅者数。
    ///
    /// 线程：必须位于 OwnerThread（EventBus 的 Drain 由宿主线程驱动，§9）。
    core::Result<std::size_t> DispatchEventToSubscribers(std::string_view name,
                                                        void (*fill)(TableWriter&, const void*),
                                                        const void* ev);

    /// 订阅某事件的脚本回调数（观测 / 测试用）。
    std::size_t SubscriberCount(std::string_view name) const noexcept;

    // ---- 诊断 ----

    LuaVM& Vm() noexcept { return *vm_; }
    const LuaVM& Vm() const noexcept { return *vm_; }

    /// 最近一次错误详情（含脚本名与行号，§15.9）。
    const LuaVM::ErrorDetail& LastError() const noexcept { return vm_->LastError(); }

    /// 把 ScriptError 映射成 core::Error（附加脚本名 / 行号 / 文本，便于排障）。
    core::Error MakeError(ScriptError code) const noexcept;

    /// 四类限制被触发的累计次数（观测）。
    std::size_t LimitHits() const noexcept { return vm_->LimitHits(); }

    /// `Call` 的非模板实现（供模板转发；也在 src 内被绑定实现复用）。
    core::Result<void> CallWithArgs(ScriptId id, std::string_view fn, const ScriptArgs& args);

    // =======================================================================
    // 内部协作接口（`_` 后缀 = 非稳定 API，仅供 src/ 内的 RAII 与绑定实现使用）
    // =======================================================================

    /// 当前脚本执行深度（>0 = 正在执行脚本）。
    std::uint32_t RunDepth() const noexcept;
    /// 当前 pending 队列长度（中止回滚的标记）。
    std::size_t PendingMark() const noexcept;
    /// 进入脚本执行区间（深度 +1）。
    void EnterRun(std::uint32_t depth_mark) noexcept;
    /// 退出脚本执行区间；`rollback_to` 非 std::size_t(-1) 时把 pending 截断回该长度
    /// —— 这是「执行被限额中止 ⇒ 本次暂存的热更作废」的回滚入口（§15.4）。
    void ExitRun(std::uint32_t depth_mark, std::size_t rollback_to) noexcept;

    /// 确保某个事件名在**真实** `core::EventBus` 上的 C++ 订阅已安装（幂等）。
    /// 由 `event.subscribe` 绑定调用；未注册事件桥的名字只依赖 BindEventApi 装的通用订阅。
    core::Result<void> EnsureEventSubscription(std::string_view name);

    /// 登记一条脚本事件订阅。`fn_ref` 为 Lua registry 引用（由绑定实现经 `luaL_ref` 取得）。
    void AddSubscriber(std::string_view name, int fn_ref);

private:
    explicit ScriptContext(std::unique_ptr<LuaVM> vm);

    /// 已加载脚本条目：模块表注册表引用（-1 = 已卸载）。
    /// 脚本的私有 `_ENV` 通过模块表的 `__index` 可达，故无需单独持有。
    struct ScriptEntry {
        ScriptId id{kInvalidScriptId};
        std::string name;
        int module_ref{-1};  ///< LUA_NOREF
    };

    /// 暂存的脚本变更（§13 Safe Point 生效）。
    struct PendingOp {
        enum class Kind : std::uint8_t { Load, Unload };
        Kind kind{Kind::Load};
        ScriptId id{kInvalidScriptId};
        std::string name;
        std::string source;   ///< Load：源码
        int chunk_ref{-1};    ///< Load：已编译 chunk 的注册表引用（编译期已校验语法）
    };

    /// 一条脚本事件订阅。
    struct Subscriber {
        std::uint64_t sub_id{0};
        int fn_ref{-1};  ///< Lua 函数注册表引用
    };

    // —— 内部工具（全部要求 OwnerThread）——

    /// 仅编译（不接受字节码）：语法错误在此立即暴露，返回 chunk 的注册表引用。
    core::Result<void> CompileChunk(std::string_view name, std::string_view source,
                                    int* out_chunk_ref);
    /// 在独立 `_ENV` 中执行已编译 chunk，返回模块表引用（失败细节写入 VM 的 LastError）。
    core::Result<void> RunCompiledChunk(std::string_view name, int chunk_ref, int* out_module_ref);
    /// 编译 + 执行（一步到位）。
    core::Result<int> RunChunkFromSource(std::string_view name, std::string_view source);

    /// 登记脚本（同名 = 热替换，id 不变）并递增版本号。
    core::Result<ScriptId> ActivateScript(std::string_view name, int module_ref);

    /// 立即卸载（释放引用 + 版本 +1）。幂等判定在调用方。
    core::Result<void> UnloadImmediate(ScriptId id);

    /// 绑定函数统一入口（所有绑定共用；靠 upvalue 1 的绑定名区分）。
    static int BindingEntry(lua_State* state) noexcept;

    /// 注册一条绑定 **并** 在沙箱里装好 Lua 入口（`ns.fn` 形式，按 '.' 拆命名空间）。
    /// 这是 Install*Bindings 的唯一落地原语：注册表 + Lua 侧一起装，避免两处不一致。
    core::Result<void> InstallBindingEntry(std::string_view name, BindingKind kind, NativeFn fn,
                                           void* user, bool read_only,
                                           std::string_view description);

    /// 命令 / 查询 op 的路由（由注册在总线上的 handler 调用）。
    core::Result<ScriptReply> RouteCommandOp(const ScriptCommand& cmd) const;
    core::Result<ScriptReply> RouteQueryOp(const ScriptQuery& query) const;

    // —— 内置绑定面的安装（实现分散在 src/bind_*.cpp）——
    core::Result<void> InstallEntityBindings();
    core::Result<void> InstallCommandBindings();
    core::Result<void> InstallQueryBindings();
    core::Result<void> InstallEventBindings();

private:
    /// 一条宿主注册的事件桥（名字 + 安装器）。
    struct EventBridge {
        std::string name;
        EventBridgeInstall install{nullptr};
        void* user{nullptr};
    };

    std::unique_ptr<LuaVM> vm_;
    ScriptServices services_{};
    BindingRegistry bindings_;

    std::vector<ScriptEntry> scripts_;                              ///< id → entry（下标 = id-1）
    std::unordered_map<std::string, ScriptId, TransparentStringHash, std::equal_to<>>
        by_name_;                                                   ///< 名 → id（热替换保 id）
    std::unordered_map<std::string, std::size_t, TransparentStringHash, std::equal_to<>>
        command_ops_;                                               ///< op → 下标
    std::vector<std::pair<ScriptCommandHandler, void*>> command_handlers_;
    std::unordered_map<std::string, std::size_t, TransparentStringHash, std::equal_to<>>
        query_ops_;
    std::vector<std::pair<ScriptQueryHandler, void*>> query_handlers_;

    std::vector<Subscriber> subscribers_;
    std::unordered_map<std::string, std::vector<std::size_t>, TransparentStringHash, std::equal_to<>>
        subs_by_name_;
    /// 已安装 C++ 侧订阅的事件名（幂等去重）。
    std::vector<std::string> installed_events_;
    /// 宿主注册的事件桥（含内置四条实体生命周期桥）。
    std::vector<EventBridge> event_bridges_;
    /// 需要在本对象析构时**退订**的 EventBus 订阅 ID。
    /// 必要性：订阅 lambda 捕获了 `this`，若总线比本对象长寿而订阅未撤，会悬垂调用。
    std::vector<core::EventBus::SubId> bridge_subs_;

    std::vector<PendingOp> pending_;
    std::uint64_t next_sub_id_{0};
    /// 执行深度（>0 表示正处在脚本执行中；热更暂存判据）。
    std::uint32_t run_depth_{0};

    /// 已激活过的脚本槽下标（观测用）。
    std::vector<std::size_t> activated_;
};

}  // namespace mmo::script
