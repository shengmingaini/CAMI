#pragma once

/// TASK-031 · 脚本 ↔ 系统 的桥接载荷（§8 绑定面：Skill / Quest 走命令，Event 走事件总线，Query 只读查询）。
///
/// 为什么需要这一层（设计取舍，§30）
/// --------------------------------
/// TASK-031 的依赖集是 `TASK-001/004/005/007/011` —— **不含** Skill（021）、Quest（019）等业务模块。
/// 因此脚本侧的 `skill.cast` / `quest.*` **不得** `#include` 业务模块头（§27.2 / §27.3）。
/// 正解：把脚本请求表达成**普通 Command / Query**，复用统一 `CommandBus` / `QueryBus` 派发，
/// 由状态 Owner（SkillSystem / QuestSystem）注册的 handler 执行：
///
///   Lua `skill.cast(caster, skill_id, target)`
///     -> `ScriptCommand{op="skill.cast", …}` -> `core::CommandBus::Dispatch`
///     -> 宿主注册的 op handler（内部调 SkillSystem）-> `ScriptReply` -> Lua 表
///
/// 这**不是**第二套消息格式（§4）：`ScriptCommand` 就是一条满足 `core::CommandLike`（5 个审计字段）
/// 的普通 Command，只是载荷为「脚本参数表 + op 名」。op 名即注册表键（§27.4：扩展靠注册，不靠 switch）。
///
/// 热路径 / 生命周期
/// ----------------
///   - `ScriptCommand` / `ScriptQuery`：`Dispatch` / `Ask` 是**调用者线程同步调用**，
///     总线不复制载荷（`RegisterFn` 擦除为 `const void*`），故 `args` 里的字符串视图
///     在整次派发期间有效 —— 零拷贝、零分配。
///   - `ScriptEvent`：EventBus **入队**（异步跨 Tick），载荷必须自持内存 →
///     全内联定长缓冲（**无指针、无 std::string**），可安全拷贝与跨 Tick 存活。

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "mmo/core/bus/command.h"
#include "mmo/core/log/log_context.h"
#include "mmo/core/log/trace_id.h"

#include "mmo/script/script_value.h"

namespace mmo::script {

/// op 名最大长度（`"quest.set_progress"` = 18；留足扩展空间）。
inline constexpr std::size_t kScriptOpMaxLen = 40;

/// 命令 / 查询的返回值：至多 2 个整数 + 1 个浮点，覆盖伤害值 / 进度 / 数量 / 对象 id 等标量语义。
/// 需要更复杂的结构时由 op 约定以 `i0` 承载对象 id，脚本再用 `entity.get` 取详情
/// —— 保持绑定面窄，避免脚本拿到任意结构导致契约失控。
struct ScriptReply {
    std::int64_t i0{0};
    std::int64_t i1{0};
    double d0{0.0};
    /// handler 是否产出了有效结果；false → 脚本侧该调用返回 nil。
    bool valid{false};
};

/// 脚本发起的受控操作（**同步**；经 `core::CommandBus` 派发给状态 Owner）。
struct ScriptCommand {
    using Result = ScriptReply;

    // —— §7 CommandLike 硬要求的 5 个审计字段（与统一 Envelope 同源，§4）——
    // 必须写 `core::` 限定：本头位于 mmo::script 命名空间，而 RequestID / PlayerID /
    // CommandSource 都定义在 mmo::core（trace_id.h / log_context.h / command.h）。
    core::RequestID request_id{core::kInvalidRequestId};
    core::PlayerID player_id{core::kInvalidPlayerId};
    core::CommandSource source{core::CommandSource::kInternal};
    std::int64_t timestamp{0};
    std::uint32_t version{1};

    // —— 载荷 ——
    char op[kScriptOpMaxLen + 1]{};
    std::uint8_t op_len{0};
    ScriptArgs args{};

    std::string_view Op() const noexcept { return std::string_view(op, op_len); }

    /// 设置 op 名；超长返回 false（**禁止静默截断** —— 截断会造成 op 路由错投）。
    bool SetOp(std::string_view name) noexcept {
        if (name.size() > kScriptOpMaxLen) {
            return false;
        }
        for (std::size_t i = 0; i < name.size(); ++i) {
            op[i] = name[i];
        }
        op_len = static_cast<std::uint8_t>(name.size());
        return true;
    }
};

/// 脚本发起的只读查询（**同步**；经 `core::QueryBus`，运行期禁止副作用，§8 / §21）。
struct ScriptQuery {
    using Result = ScriptReply;

    core::RequestID request_id{core::kInvalidRequestId};

    char op[kScriptOpMaxLen + 1]{};
    std::uint8_t op_len{0};
    ScriptArgs args{};

    std::string_view Op() const noexcept { return std::string_view(op, op_len); }

    bool SetOp(std::string_view name) noexcept {
        if (name.size() > kScriptOpMaxLen) {
            return false;
        }
        for (std::size_t i = 0; i < name.size(); ++i) {
            op[i] = name[i];
        }
        op_len = static_cast<std::uint8_t>(name.size());
        return true;
    }
};

// —— op handler（宿主注册；内部调业务系统）——
// fn + user：注册期一次、调用期零分配（§10）。
using ScriptCommandHandler = core::Result<ScriptReply> (*)(const ScriptCommand& cmd, void* user);
using ScriptQueryHandler = core::Result<ScriptReply> (*)(const ScriptQuery& query, void* user);

/// 脚本可见事件的自持内存上限（超出即拒绝发布，**禁止截断**）。
inline constexpr std::size_t kScriptEventMaxNameLen = 32;
inline constexpr std::size_t kScriptEventMaxArgs = 4;
inline constexpr std::size_t kScriptEventMaxStrLen = 48;

/// 脚本可见事件：经**真实** `core::EventBus` 发布、异步派发（§8「经 EventBus，异步」）。
///
/// 与 `ScriptCommand` 的关键差别：EventBus 会把事件**入队**，所以载荷必须自持内存。
/// 本类型全部字段为平凡类型定长缓冲 → 可平凡拷贝、无悬挂、跨 Tick 安全。
/// `kCritical = false`：非关键事件在队列满时按 §15.5 背压策略丢弃并计数，不阻塞 Tick。
struct ScriptEvent {
    static constexpr bool kCritical = false;

    char name[kScriptEventMaxNameLen + 1]{};
    std::uint8_t name_len{0};

    std::int64_t ints[kScriptEventMaxArgs]{};
    char texts[kScriptEventMaxArgs][kScriptEventMaxStrLen + 1]{};
    std::uint8_t text_lens[kScriptEventMaxArgs]{};
    std::uint8_t kinds[kScriptEventMaxArgs]{};  ///< ScriptValueType
    std::uint8_t argc{0};

    std::string_view Name() const noexcept { return std::string_view(name, name_len); }
    std::size_t ArgCount() const noexcept { return argc; }

    /// 还原第 i 个参数（越界返回 Nil）。
    ScriptValue Arg(std::size_t i) const noexcept {
        if (i >= argc) {
            return ScriptValue::Nil();
        }
        switch (static_cast<ScriptValueType>(kinds[i])) {
            case ScriptValueType::Bool:
                return ScriptValue::Bool(ints[i] != 0);
            case ScriptValueType::Integer:
                return ScriptValue::Int(ints[i]);
            case ScriptValueType::Number:
                return ScriptValue::Num(static_cast<double>(ints[i]) / 1000.0);
            case ScriptValueType::String:
                return ScriptValue::Str(
                    std::string_view(texts[i], text_lens[i]));
            case ScriptValueType::Nil:
                break;
        }
        return ScriptValue::Nil();
    }

    /// 设置事件名；超长返回 false（禁止截断）。
    bool SetName(std::string_view v) noexcept {
        if (v.size() > kScriptEventMaxNameLen) {
            return false;
        }
        for (std::size_t i = 0; i < v.size(); ++i) {
            name[i] = v[i];
        }
        name_len = static_cast<std::uint8_t>(v.size());
        return true;
    }

    /// 追加一个标量参数；容量满或字符串超长返回 false（禁止截断）。
    bool PushArg(ScriptValue v) noexcept {
        if (argc >= kScriptEventMaxArgs) {
            return false;
        }
        const auto idx = static_cast<std::size_t>(argc);
        kinds[idx] = static_cast<std::uint8_t>(v.Type());
        switch (v.Type()) {
            case ScriptValueType::Nil:
                ints[idx] = 0;
                break;
            case ScriptValueType::Bool:
                ints[idx] = v.AsBool() ? 1 : 0;
                break;
            case ScriptValueType::Integer:
                ints[idx] = v.AsInt();
                break;
            case ScriptValueType::Number:
                // 以千分之一为单位定点存储，避免整数槽丢精度（事件载荷不承载高精度科学计算）。
                ints[idx] = static_cast<std::int64_t>(v.AsNum() * 1000.0);
                break;
            case ScriptValueType::String: {
                const std::string_view s = v.AsStr();
                if (s.size() > kScriptEventMaxStrLen) {
                    return false;
                }
                for (std::size_t i = 0; i < s.size(); ++i) {
                    texts[idx][i] = s[i];
                }
                text_lens[idx] = static_cast<std::uint8_t>(s.size());
                break;
            }
        }
        ++argc;
        return true;
    }
};

// 事件必须能被 EventBus 内联/堆路径安全搬运（event_slot.h 的静态约束）。
static_assert(std::is_nothrow_destructible_v<ScriptEvent>, "ScriptEvent 必须 nothrow 可析构");
static_assert(alignof(ScriptEvent) <= 8, "ScriptEvent 对齐必须 <= 8（EventBus 内联缓冲约束）");
static_assert(std::is_trivially_copyable_v<ScriptEvent>, "ScriptEvent 必须可平凡拷贝（跨 Tick 入队）");

}  // namespace mmo::script
