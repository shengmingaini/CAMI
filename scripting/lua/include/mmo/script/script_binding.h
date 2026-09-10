#pragma once

/// TASK-031 · 脚本绑定层（§8 绑定面，§27.4 注册表机制）。
///
/// 三段式设计
/// ----------
///   1. `BindingRegistry` —— 脚本名 → 原生函数。**扩展靠 Add 注册，不在 switch 里穷举**
///      （§27.4）；宿主可注册自己的绑定而不修改本模块任何文件。
///   2. `ScriptCall` —— 一次脚本调用的上下文（参数、返回值、系统接口）。
///   3. `ScriptServices` —— `Bind*Api` 注入的上游系统接口集（Entity/Event/Query/Command）。
///
/// 值（§30 Correctness 优先）
/// -------------------------
///   - 绑定函数用 **fn + user 的函数指针**（非 std::function）：注册期一次、调用期零分配，
///     满足 §10 热路径要求。带捕获的需求用 `user` 指针承载状态。
///   - 副作用纪律（§8 / §21）：`query.*` 类绑定必须 `read_only = true`，只允许读；
///     任何写操作必须走 `skill.*` / `quest.*` / `entity.set_hp` 这类**命令**路径，
///     由状态 Owner 执行 —— 脚本永远拿不到裸指针。

#include <cstddef>
#include <cstdint>
#include <functional>
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

#include "mmo/script/script_value.h"

namespace mmo::script {

class ScriptContext;  // 前置声明（friend / 嵌套）

/// 异构查找哈希：允许 `unordered_map<std::string, …>` 直接用 `std::string_view` 查，
/// 不产生临时 `std::string`（C++20 `is_transparent` 机制）。热路径查找零分配。
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view v) const noexcept {
        return std::hash<std::string_view>{}(v);
    }
    std::size_t operator()(const std::string& v) const noexcept {
        return std::hash<std::string_view>{}(v);
    }
};

/// `Bind*Api` 注入的上游系统接口集合。
///
/// 全部为**非拥有**指针，生命周期由宿主（Scene / Tick 装配）保证长于 ScriptContext。
/// 未绑定的项为 nullptr —— 绑定实现必须显式判空并返回 `INVALID_ARGUMENT`，
/// 禁止解引用空指针（§19「脚本调用不存在的 API 返回明确错误而非崩溃」）。
struct ScriptServices {
    game::EntityManager* entities{nullptr};
    core::EventBus* events{nullptr};
    core::QueryBus* queries{nullptr};
    core::CommandBus* commands{nullptr};
};

/// 绑定分类：与 §8 绑定面五类一一对应（`Native` 为宿主扩展位）。
enum class BindingKind : std::uint8_t {
    Entity = 0,
    Skill = 1,
    Quest = 2,
    Event = 3,
    Query = 4,
    Native = 5,
};

const char* ToString(BindingKind kind) noexcept;

/// 一次脚本调用的上下文（**Hot Path**；仅在同步调用期间有效，禁止跨 Tick 持有）。
class ScriptCall {
public:
    /// 被调用的脚本可见名，如 `"entity.get"` / `"skill.cast"`。
    std::string_view Name() const noexcept { return name_; }
    std::size_t ArgCount() const noexcept { return arg_count_; }

    /// 第 i 个参数（0-based）转标量；table / function 返回 `ScriptValue::Nil()`。
    /// 越界返回 Nil（禁止越界读）。
    ScriptValue Arg(std::size_t i) const noexcept;
    ScriptValue ArgOr(std::size_t i, ScriptValue dflt) const noexcept;

    bool ArgIsTable(std::size_t i) const noexcept;
    bool ArgIsString(std::size_t i) const noexcept;
    bool ArgIsInteger(std::size_t i) const noexcept;

    /// 表参数视图；非表返回 `Valid() == false` 的空视图。
    ScriptTableView ArgTable(std::size_t i) const noexcept;

    /// 第 i 个参数在 Lua 栈上的绝对索引（越界返回 -1）。
    ///
    /// 只给需要**直接操作 Lua 值**的绑定使用（典型场景：`event.subscribe(name, fn)`
    /// 需要把函数对象存成注册表引用）。常规绑定请用 `Arg` / `ArgTable`。
    int ArgStackIndex(std::size_t i) const noexcept;

    /// 设置标量返回值。未设置 = 该次调用不返回结果（Lua 侧为 nil）。
    void SetResult(ScriptValue v) noexcept {
        result_ = v;
        has_result_ = true;
    }
    void SetResultNil() noexcept { SetResult(ScriptValue()); }

    /// 把返回值写成一整张 Lua 表：`fill` 用 `TableWriter` 逐字段填充。
    /// `user` 原样透传给 `fill`（用于携带事件对象等）。
    void SetResultTable(void (*fill)(TableWriter&, void*), void* user);

    const ScriptValue& Result() const noexcept { return result_; }
    bool HasResult() const noexcept { return has_result_; }

    /// 绑定实现的统一出口：把已设置的返回值压到 Lua 栈上，返回结果个数。
    /// 约定写法：`core::Result<int> MyBinding(ScriptCall& call, void*) { … return call.Done(); }`
    core::Result<int> Done() const;

    /// 注入的上游系统接口（只读）。
    const ScriptServices& Services() const noexcept { return services_; }

    /// 拥有本调用的 ScriptContext（用于嵌套能力，如事件派发）。
    ScriptContext& Context() const noexcept { return ctx_; }

    /// 直接写 Lua error 消息（`RaiseError` 会经 `lua_error` 抛出，走 Lua 错误机制而非 C++ 异常）。
    ///
    /// **UB 防护**：调用方必须确保进入本函数时**没有任何存活中的非平凡局部对象**
    /// （Lua 以 `longjmp` 实现错误抛出，会跳过 C++ 析构）。本模块的绑定实现统一用
    /// `MMO_SCRIPT_BIND` 宏保证这一点；宿主自写绑定时也必须遵守。
    [[noreturn]] void RaiseError(std::string_view message) const;

private:
    friend class ScriptContext;
    ScriptCall(ScriptContext& ctx, std::string_view name, void* state, int arg_base,
               std::size_t arg_count, const ScriptServices& services) noexcept
        : ctx_(ctx), name_(name), services_(services), state_(state),
          arg_base_(arg_base), arg_count_(arg_count) {}

    /// 供 ScriptContext 取回由 SetResultTable 建好的表所在栈索引。
    int ResultTableIndex() const noexcept { return result_table_index_; }

    ScriptContext& ctx_;
    std::string_view name_;
    ScriptServices services_{};
    void* state_{nullptr};  ///< lua_State*（不透明）
    int arg_base_{0};       ///< 第 0 个参数在 Lua 栈上的绝对索引
    std::size_t arg_count_{0};
    ScriptValue result_{};
    bool has_result_{false};
    int result_table_index_{-1};
};

/// 原生绑定函数签名（fn + user：零分配热路径；`user` 为宿主状态）。
using NativeFn = core::Result<int> (*)(ScriptCall& call, void* user);

/// 一条绑定的静态描述。
struct BindingDef {
    std::string name;        ///< 脚本可见名（如 "entity.get"）
    BindingKind kind{BindingKind::Native};
    NativeFn fn{nullptr};
    void* user{nullptr};
    /// 只读标志：Query 类必须为 true。运行期由 `ScriptCall` 校验（写操作误标只读 = 契约违约）。
    bool read_only{false};
    std::string description;  ///< 人类可读说明（写入 INTERFACE 报告 / 排障）
};

/// 绑定注册表：脚本名 → 绑定定义。
///
/// 并发模型（§9）：注册发生在**装配期**（单线程），查找发生在 Tick 热路径。
/// 装配完成后不再 Add，因此查找路径无锁；Add 在运行期调用会返回 `BUSY`
/// （防止「Tick 中途改绑定表」这种会让脚本行为漂移的修改）。
class BindingRegistry {
public:
    BindingRegistry() = default;
    BindingRegistry(const BindingRegistry&) = delete;
    BindingRegistry& operator=(const BindingRegistry&) = delete;

    /// 注册绑定。重名返回 `INVALID_ARGUMENT`（**禁止静默覆盖**，与 TASK-007 总线一致）；
    /// 空名 / 空函数指针同样拒绝。冻结后调用返回 `BUSY`。
    core::Result<void> Add(BindingDef def);

    /// 冻结注册表（装配期结束；此后 Add 返回 BUSY）。
    void Freeze() noexcept { frozen_ = true; }
    bool Frozen() const noexcept { return frozen_; }

    core::Result<const BindingDef*> Find(std::string_view name) const noexcept;
    bool Contains(std::string_view name) const noexcept;
    std::size_t Size() const noexcept { return defs_.size(); }
    std::size_t SizeOf(BindingKind kind) const noexcept;

    /// 全部绑定（供文档生成 / 报告；顺序 = 注册顺序）。
    const std::vector<BindingDef>& All() const noexcept { return defs_; }

private:
    std::vector<BindingDef> defs_;
    std::unordered_map<std::string, std::size_t, TransparentStringHash, std::equal_to<>>
        index_;
    bool frozen_{false};
};

/// 绑定实现统一出口宏（**UB 防护 + 错误映射**）。
///
/// 用法：
/// ```cpp
/// static core::Result<int> Impl(ScriptCall& call, void* user) { … }
/// static int LuaEntry(lua_State* L) {
///     MMO_SCRIPT_BIND_ENTER();          // 构造 call / 取回 ScriptContext
///     MMO_SCRIPT_BIND_BODY(Impl(call, user));
/// }
/// ```
/// `MMO_SCRIPT_BIND_BODY` 保证在 `lua_error`（longjmp）之前让所有非平凡局部对象析构完毕，
/// 并把失败信息转成带位置前缀的 Lua error。
#define MMO_SCRIPT_BIND_BODY(EXPR)                                                   \
    do {                                                                             \
        char mmo_bind_msg[192];                                                      \
        std::size_t mmo_bind_len = 0;                                                \
        int mmo_bind_nres = 0;                                                       \
        bool mmo_bind_ok = false;                                                    \
        {                                                                            \
            const auto mmo_bind_r = (EXPR);                                           \
            mmo_bind_ok = static_cast<bool>(mmo_bind_r);                              \
            if (mmo_bind_ok) {                                                        \
                mmo_bind_nres = mmo_bind_r.Value();                                   \
            } else {                                                                 \
                mmo_bind_len = ::mmo::script::detail::CopyErrorText(                   \
                    mmo_bind_r.Err(), mmo_bind_msg, sizeof(mmo_bind_msg));            \
            }                                                                        \
        }                                                                            \
        if (mmo_bind_ok) {                                                            \
            return mmo_bind_nres;                                                     \
        }                                                                            \
        lua_pushlstring(L, mmo_bind_msg, mmo_bind_len);                               \
        return lua_error(L);                                                          \
    } while (0)

namespace detail {

/// 把错误文本复制成**平凡类型**缓冲（供 longjmp 前使用），返回写入长度。
std::size_t CopyErrorText(const core::Error& err, char* out, std::size_t cap) noexcept;

}  // namespace detail

}  // namespace mmo::script
