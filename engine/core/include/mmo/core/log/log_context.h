#pragma once

#include <string_view>
#include <utility>

#include "mmo/core/log/trace_id.h"

namespace mmo::core {

// ---------------------------------------------------------------------------
// 业务 ID 占位别名（模块边界说明）
// 全仓目前（TASK-002 时点）尚无统一 ID 头，而 §7 的接口契约要求 LogContext 暴露
// PlayerID / SceneID / kInvalidPlayerId / kInvalidSceneId 四个符号。
// 这里在 log 子树内给出最小占位定义；后续统一 ID 头落地后，只需把下面四行替换为
// include 该头并保留同名符号，LogContext 的字段类型与语义不变（非破坏性变更）。
// ---------------------------------------------------------------------------
using PlayerID = std::uint64_t;
using SceneID = std::uint64_t;
inline constexpr PlayerID kInvalidPlayerId = 0;
inline constexpr SceneID kInvalidSceneId = 0;

/// 日志上下文：随调用链自动传播的一组维度。
///
/// 存储于 thread_local，读取成本≈一次 TLS 访问（无锁、无原子）。
/// 字段为哨兵值（0 / kInvalid*）时表示「未设置」，ScopedLogContext 依此做逐字段覆盖，
/// 因此嵌套设置时未指定的字段会继承外层上下文，而不是被清空。
struct LogContext {
    TraceID trace_id{kInvalidTraceId};
    RequestID request_id{kInvalidRequestId};
    PlayerID player_id{kInvalidPlayerId};
    SceneID scene_id{kInvalidSceneId};

    /// 模块名，**必须为静态存储期且以 '\0' 结尾的字符串字面量**（如 "skill"）。
    /// 禁止运行时拼接：拼接会产生堆分配，直接违反 §22「单条日志零堆分配」。
    std::string_view module{};
};

/// 当前线程的日志上下文（只读引用，禁止跨线程传递该引用）。
const LogContext& CurrentLogContext() noexcept;

/// 整体替换标记：WithContext 跨线程携带时使用，不做哨兵值判断。
struct ReplaceAllTag {
    explicit ReplaceAllTag() = default;
};
inline constexpr ReplaceAllTag kReplaceAll{};

/// RAII 覆盖 / 恢复当前线程日志上下文，析构即恢复，异常安全。
///
/// 默认构造按「哨兵值」逐字段覆盖：patch 中等于哨兵值的字段保持外层不变。
/// 传 kReplaceAll 时整体替换（用于跨线程上下文迁移）。
class ScopedLogContext final {
public:
    explicit ScopedLogContext(const LogContext& patch) noexcept;
    ScopedLogContext(const LogContext& full, ReplaceAllTag) noexcept;
    ~ScopedLogContext();

    ScopedLogContext(const ScopedLogContext&) = delete;
    ScopedLogContext& operator=(const ScopedLogContext&) = delete;
    ScopedLogContext(ScopedLogContext&&) = delete;
    ScopedLogContext& operator=(ScopedLogContext&&) = delete;

private:
    LogContext saved_;
};

/// 跨线程携带上下文：把「别处的 LogContext」整体套用到当前线程并执行 fn。
///
/// 用法（TASK-002 §17 集成测试场景）：
///   auto ctx = mmo::core::CurrentLogContext();        // 主线程快照（值拷贝）
///   pool.Post([ctx]{ mmo::core::WithContext(ctx, []{ MMO_LOG(INFO, "in worker"); }); });
///
/// 契约：跨线程提交任务时必须显式拷贝上下文，禁止依赖 thread_local 自动延续。
template <typename Fn>
decltype(auto) WithContext(const LogContext& carried, Fn&& fn) {
    ScopedLogContext scoped(carried, kReplaceAll);
    return std::forward<Fn>(fn)();
}

}  // namespace mmo::core
