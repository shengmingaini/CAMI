// server/gateway/src/resilience/health_monitor.cpp — TASK-037 §7 / §15.1-15.2
//
// 节点健康 SLA 监控 + 替换节点选择。
//
// 关键设计（§27.3 模块边界 + 不重复发布 NodeDead）：
//   - 本 Monitor 维护**自己的**健康权威表 nodes_，由 Tick 驱动三态迁移。
//   - NodeRegistry（TASK-010）在此仅作为「节点目录 + 一致性哈希 Pick」复用，
//     其自身的 Tick 在本路径下**不被调用**，因此 NodeDead 仅由本 Monitor 发布一次，
//     不会出现「两个 Tick 各发一次」的重复事件。
//   - 节点被判 Dead 时：①发布 NodeDead（供 37.3 FailoverCoordinator 消费）；
//     ②主动从 NodeRegistry 目录剔除（registry_.Unregister），保证路由不再选中。
//   - 整个状态机单写者（Gateway 主线程 Tick），无锁。

#include "mmo/gateway/resilience/health_monitor.h"

#include <algorithm>

namespace mmo::gateway {
namespace {

using core::MonotonicClock;

}  // namespace

const char* ToString(NodeStatus status) noexcept {
    switch (status) {
        case NodeStatus::Healthy: return "Healthy";
        case NodeStatus::Suspect: return "Suspect";
        case NodeStatus::Dead:    return "Dead";
        default:                  return "Unknown";
    }
}

HealthMonitor::HealthMonitor(core::EventBus* bus)
    : bus_(bus), config_(), registry_(bus) {}

HealthMonitor::HealthMonitor(core::EventBus* bus, HealthMonitorConfig config)
    : bus_(bus), config_(std::move(config)), registry_(bus) {}

core::Result<void> HealthMonitor::Register(const NodeInfo& info) {
    auto r = registry_.Register(info);
    if (!r.HasValue()) {
        return core::Result<void>::Fail(r.Err());
    }
    NodeHealthInfo h;
    h.id             = info.id;
    h.role           = info.role;
    h.last_heartbeat = MonotonicClock::Point();  // 登记即视为「此刻健康」
    h.load           = info.load;
    h.missed         = 0;
    h.status         = NodeStatus::Healthy;
    nodes_[info.id]  = h;
    return core::Result<void>::Ok();
}

core::Result<void> HealthMonitor::Heartbeat(NodeId id, std::uint32_t load) {
    auto r = registry_.Heartbeat(id, load);
    if (!r.HasValue()) {
        return core::Result<void>::Fail(r.Err());
    }
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        // 目录有、本地无快照（理论上 Register 已建）：补建，保证两边一致。
        NodeHealthInfo h{};
        h.id             = id;
        h.role           = NodeRole::Unknown;
        h.last_heartbeat = MonotonicClock::Point();
        h.load           = load;
        h.missed         = 0;
        h.status         = NodeStatus::Healthy;
        nodes_[id]       = h;
        return core::Result<void>::Ok();
    }
    NodeHealthInfo& h  = it->second;
    h.last_heartbeat  = MonotonicClock::Point();
    h.load            = load;
    h.missed          = 0;
    if (h.status == NodeStatus::Suspect) {
        h.status = NodeStatus::Healthy;  // 恢复
    }
    return core::Result<void>::Ok();
}

core::Result<void> HealthMonitor::Unregister(NodeId id) {
    nodes_.erase(id);
    (void)registry_.Unregister(id);  // 幂等：目录里没有也返回 Ok（本地已清）
    return core::Result<void>::Ok();
}

core::Result<void> HealthMonitor::Tick(core::SteadyTime now) {
    const std::int64_t interval_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config_.heartbeat_interval)
            .count();
    if (interval_ns <= 0) {
        return core::Result<void>::Ok();
    }

    std::vector<NodeId> dead;
    for (auto& kv : nodes_) {
        NodeHealthInfo& h = kv.second;
        if (h.status == NodeStatus::Dead) {
            continue;  // 已处理，不再重复发布
        }
        const std::int64_t elapsed = MonotonicClock::Elapsed(h.last_heartbeat, now);
        if (elapsed < 0) {
            continue;  // 时钟异常保护（MonotonicClock 正常不会回拨）
        }
        const std::uint32_t missed =
            static_cast<std::uint32_t>(elapsed / interval_ns);
        if (missed == h.missed) {
            continue;  // 同一心跳窗内，无需重复处理
        }
        h.missed = missed;
        if (missed >= config_.dead_after_misses) {
            h.status = NodeStatus::Dead;
            dead.push_back(h.id);
        } else if (missed >= config_.suspect_after_misses) {
            h.status = NodeStatus::Suspect;
        }
    }

    // 判定 Dead：发布 NodeDead（仅一次）+ 从目录剔除，停止后续路由。
    for (const NodeId id : dead) {
        auto it = nodes_.find(id);
        const NodeRole role =
            (it != nodes_.end()) ? it->second.role : NodeRole::Unknown;
        if (bus_ != nullptr) {
            (void)bus_->Publish(NodeDead{id, role, now});
        }
        (void)registry_.Unregister(id);
    }
    return core::Result<void>::Ok();
}

NodeStatus HealthMonitor::StatusOf(NodeId id) const noexcept {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return NodeStatus::Dead;  // 未登记视为不可用
    }
    return it->second.status;
}

NodeRole HealthMonitor::RoleOf(NodeId id) const noexcept {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return NodeRole::Unknown;
    }
    return it->second.role;
}

std::size_t HealthMonitor::DeadCount() const noexcept {
    std::size_t c = 0;
    for (const auto& kv : nodes_) {
        if (kv.second.status == NodeStatus::Dead) {
            ++c;
        }
    }
    return c;
}

core::Result<std::vector<NodeId>> HealthMonitor::FindReplacement(
    NodeRole role, std::string_view affinity) {
    struct Cand {
        NodeId       id;
        std::uint32_t load;
    };
    std::vector<Cand> cands;
    cands.reserve(nodes_.size());
    for (const auto& kv : nodes_) {
        const NodeHealthInfo& h = kv.second;
        if (h.role == role && h.status != NodeStatus::Dead) {
            cands.push_back({h.id, h.load});
        }
    }
    // 低负载优先；负载相同按 id 升序，保证确定性（测试可断言）。
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.load != b.load) {
            return a.load < b.load;
        }
        return a.id < b.id;
    });

    if (!cands.empty()) {
        std::vector<NodeId> out;
        out.reserve(cands.size());
        for (const auto& c : cands) {
            out.push_back(c.id);
        }
        return core::Result<std::vector<NodeId>>::Ok(std::move(out));
    }

    // 本地无候选：若提供 affinity，退而求其次用一致性哈希 Pick。
    if (affinity.empty()) {
        return core::Result<std::vector<NodeId>>::Ok(std::vector<NodeId>{});
    }
    auto pick = registry_.Pick(role, affinity);
    if (!pick.HasValue()) {
        // 全部不可用（BUSY）：返回空，交由调用方决策，不视为错误。
        return core::Result<std::vector<NodeId>>::Ok(std::vector<NodeId>{});
    }
    return core::Result<std::vector<NodeId>>::Ok(
        std::vector<NodeId>{pick.Value()});
}

}  // namespace mmo::gateway
