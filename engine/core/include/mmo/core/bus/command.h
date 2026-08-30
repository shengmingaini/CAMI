#pragma once

/// TASK-007 · Command / Query / Event Bus —— Command 与 Query 的统一上下文与概念约束。
///
/// 设计要点（§4 State Owner / §8 Data Model）：
///   - Command：「请执行一个操作」，允许副作用，必须可审计（必带 5 个审计字段）；
///   - Query  ：「读取数据」，**禁止**任何副作用（§21 Forbidden）；
///   - Event  ：「已发生的事实」，异步派发，见 event_bus.h。
///
/// 总线只搬运、不拥有业务状态：Context 是**只读入参**，业务状态一律由各模块自己持有，
/// 跨模块写入必须走 Command，禁止直接改对方内存（PROJECT_REQUIREMENTS §10 / §12）。

#include <concepts>
#include <cstdint>

#include "mmo/core/log/log_context.h"
#include "mmo/core/log/trace_id.h"

namespace mmo::core {

/// 命令来源：决定审计与权限校验路径（客户端发起的必须走合法性校验）。
enum class CommandSource : std::uint8_t {
    kInternal = 0,  ///< 服务器内部（Tick / 定时器 / 事件订阅者）
    kClient = 1,    ///< 客户端请求
    kRpc = 2,       ///< 跨进程 RPC
    kConsole = 3,   ///< 运维控制台 / GM 指令
};

/// 名称化（日志/审计用）；未知值返回 "UNKNOWN"，禁止崩溃。
const char* ToString(CommandSource source) noexcept;

/// Command 执行上下文：随调用链透传，**只读**。
///
/// trace_id / request_id 复用 TASK-002 的 ID 语义（与 TASK-005 Envelope 同源），
/// 因此总线不需要依赖 protocol 模块 —— 保持依赖方向单向（Core 不反向依赖 Protocol）。
struct CommandContext {
    TraceID trace_id{kInvalidTraceId};
    RequestID request_id{kInvalidRequestId};
    PlayerID player_id{kInvalidPlayerId};
    SceneID scene_id{kInvalidSceneId};
    CommandSource source{CommandSource::kInternal};
};

/// Query 执行上下文：字段与 CommandContext 对齐，但**不含 source**
/// —— 查询无副作用，来源不影响结果，也就无需参与审计。
struct QueryContext {
    TraceID trace_id{kInvalidTraceId};
    RequestID request_id{kInvalidRequestId};
    PlayerID player_id{kInvalidPlayerId};
    SceneID scene_id{kInvalidSceneId};
};

/// Command 概念约束（§15.1）：必须含 5 个审计字段 + 内嵌 Result 类型。
///
/// 用 concept 而非基类：命令是纯值类型（可拷贝入队、可序列化），不引入虚表开销。
/// 经济类命令另有 TransactionID / IdempotencyKey 要求（§8），由各自模块自行追加字段。
template <typename C>
concept CommandLike = requires(const C& c) {
    typename C::Result;
    { c.request_id } -> std::convertible_to<RequestID>;
    { c.player_id } -> std::convertible_to<PlayerID>;
    { c.source } -> std::convertible_to<CommandSource>;
    { c.timestamp } -> std::convertible_to<std::int64_t>;
    { c.version } -> std::convertible_to<std::uint32_t>;
};

/// Query 概念约束：必须声明返回类型 + request_id（用于串联追踪）。
template <typename Q>
concept QueryLike = requires(const Q& q) {
    typename Q::Result;
    { q.request_id } -> std::convertible_to<RequestID>;
};

}  // namespace mmo::core
