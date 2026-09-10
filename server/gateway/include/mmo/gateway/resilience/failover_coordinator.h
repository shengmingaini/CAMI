// server/gateway/include/mmo/gateway/resilience/failover_coordinator.h — TASK-037 §7 / §15.6-15.7
//
// GameNode 故障接管编排：OnNodeDead → 批量失效路由 → 选同角色低负载替换节点 →
// 批量 Reattach。带并发限速（单次故障最多 max_concurrent 个会话同时迁移，其余排队），
// 防雪崩（§19 / §21 Forbidden）。
//
// 设计（§27.3 模块边界）：只消费 TASK-010 NodeRegistry（经 37.1 HealthMonitor 的公开接口）
// 与 TASK-009 SessionManager（经注入的 ISessionRebinder 间接调用）。会话目录 / 重绑定通过
// 本任务定义的窄接口（ISessionDirectory / ISessionRebinder）注入，禁止 include 上游 src/。
//
// 线程模型（§9）：由 Gateway 主线程驱动，无锁（单写者）。Drain 可跨多帧调用以摊薄迁移压力。

#pragma once

#include <cstdint>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/gateway/session/session.h"
#include "mmo/gateway/resilience/health_monitor.h"

namespace mmo::gateway {

/// 会话目录（只读）：供故障接管查询某节点上的在役会话。
class ISessionDirectory {
public:
    virtual ~ISessionDirectory() = default;
    virtual std::vector<SessionId> SessionsOnNode(NodeId node) const noexcept = 0;
};

/// 会话重绑定（到新 GameNode）：真实实现走 SessionManager + PlayerRouter（本任务不修改 DONE 模块）。
class ISessionRebinder {
public:
    virtual ~ISessionRebinder() = default;
    virtual core::Result<void> Rebind(SessionId id, NodeId to) noexcept = 0;
};

struct FailoverStats {
    std::size_t   detected{0};       // 检测到的 Dead 节点数
    std::size_t   reattached{0};     // 成功迁移的会话数
    std::size_t   failed{0};         // 迁移失败数
    std::int64_t  duration_ms{0};    // 最近一次接管耗时（单调时钟）
};

/// GameNode 故障接管编排（§7 / §15.6）。
class FailoverCoordinator {
public:
    FailoverCoordinator(HealthMonitor& health, ISessionDirectory& dir,
                        ISessionRebinder& rebinder, std::size_t max_concurrent = 64);

    /// 节点被判 Dead：找同角色替换节点，批量迁移其会话。
    core::Result<void> OnNodeDead(NodeId dead, core::TraceID trace);

    /// 把 from 上的会话迁移到 to（带并发限速）。可重复调用以跨帧 Drain 剩余队列。
    core::Result<void> ReattachPlayers(NodeId from, NodeId to, core::TraceID trace);

    FailoverStats Stats() const noexcept { return stats_; }

    /// 当前在途（已发起未完成的）迁移数 —— 限速观测点。
    std::size_t InFlight() const noexcept { return in_flight_; }
    /// 待迁移队列深度。
    std::size_t QueueDepth() const noexcept { return queue_.size(); }

private:
    void Drain(core::TraceID trace);

    HealthMonitor&     health_;
    ISessionDirectory& dir_;
    ISessionRebinder&  rebinder_;
    std::size_t        max_concurrent_;
    FailoverStats      stats_;

    NodeId             current_to_{0};
    std::vector<SessionId> queue_;   // 待迁移会话（限速分批）
    std::size_t        in_flight_{0};
};

}  // namespace mmo::gateway
