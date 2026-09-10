// server/gateway/include/mmo/gateway/resilience/health_monitor.h — TASK-037 §7 / §15.1-15.2
//
// 节点健康 SLA 监控 + 替换节点选择。
//
// 设计（§27.3 模块边界）：仅消费 TASK-010 NodeRegistry 的**公开接口**（节点目录 +
// 一致性哈希 Pick / ListHealthy），自身维护健康状态机（3 次心跳丢失 → Dead）。
// 不 include 任何上游 src/，不修改 TASK-010 接口。NodeRegistry 是「节点目录 + 路由」，
// 本 Monitor 是「健康 SLA 权威」——两者职责分离，Dead 时本 Monitor 主动把节点从目录剔除，
// 保证路由不再选中已死节点。
//
// 线程模型（§9）：由 Gateway 主线程 Tick 驱动，无锁（单写者）。

#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/route/node_registry.h"

namespace mmo::gateway {

/// 节点健康三态（与 NodeRegistry::NodeHealth 对齐：Healthy / Suspect / Dead）。
enum class NodeStatus : std::uint8_t {
    Healthy = 0,  // 正常
    Suspect = 1,  // 1 次心跳丢失（谨慎路由）
    Dead    = 2,  // 3 次丢失（剔除，停止路由，发布 NodeDead）
};

const char* ToString(NodeStatus status) noexcept;

/// 单节点健康快照（§7 NodeHealth）。
/// 命名刻意用 NodeHealthInfo，避免与 TASK-010 node_registry.h 的
/// `enum class NodeHealth` 在同一命名空间冲突。
struct NodeHealthInfo {
    NodeId           id{0};
    NodeRole         role{NodeRole::GameNode};
    core::SteadyTime last_heartbeat{};
    std::uint32_t    missed{0};
    std::uint32_t    load{0};
    NodeStatus       status{NodeStatus::Healthy};
};

struct HealthMonitorConfig {
    core::DurationMs heartbeat_interval{5000};  // 期望心跳间隔
    std::uint32_t    suspect_after_misses{1};   // 丢 1 次 → Suspect
    std::uint32_t    dead_after_misses{3};      // 丢 3 次 → Dead
};

/// 节点健康监控器 + 替换节点选择。
class HealthMonitor {
public:
    using Config = HealthMonitorConfig;

    explicit HealthMonitor(core::EventBus* bus = nullptr);
    HealthMonitor(core::EventBus* bus, HealthMonitorConfig config);

    /// 注册 / 更新节点：转发到 NodeRegistry，并建立本地健康快照。
    core::Result<void> Register(const NodeInfo& info);
    /// 心跳上报：刷新本地快照 + 转发目录。
    core::Result<void> Heartbeat(NodeId id, std::uint32_t load);
    /// 主动注销：本地 + 目录同步移除。
    core::Result<void> Unregister(NodeId id);

    /// 心跳超时扫描：missed++；Suspect / Dead 迁移；Dead 时发布 NodeDead 并从目录剔除。
    core::Result<void> Tick(core::SteadyTime now);

    /// 节点当前健康状态（未登记视为不可用 → Dead）。
    NodeStatus StatusOf(NodeId id) const noexcept;

    /// 节点角色（未登记返回 Unknown）。供 FailoverCoordinator 选同角色替换节点。
    NodeRole RoleOf(NodeId id) const noexcept;

    /// 替换节点选择：同角色、未 Dead 的节点按负载升序返回（低负载优先）；
    /// 若本地无候选且提供 affinity，退而求其次用一致性哈希 Pick。
    core::Result<std::vector<NodeId>> FindReplacement(NodeRole role,
                                                     std::string_view affinity = {});

    std::size_t Size() const noexcept { return nodes_.size(); }
    std::size_t DeadCount() const noexcept;

private:
    core::EventBus*      bus_;
    HealthMonitorConfig  config_;
    NodeRegistry         registry_;  // 节点目录 + 一致性哈希 Pick（复用 TASK-010）
    std::unordered_map<NodeId, NodeHealthInfo> nodes_;  // 本地健康权威
};

}  // namespace mmo::gateway
