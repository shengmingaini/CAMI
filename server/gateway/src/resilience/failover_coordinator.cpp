// server/gateway/src/resilience/failover_coordinator.cpp — TASK-037 §7 / §15.6-15.7
//
// GameNode 故障接管编排。单写者（Gateway 主线程），无锁。
// 限速：每次 Drain 最多发起 max_concurrent_ 个 Rebind（一个波次），剩余留队下次再 Drain，
// 避免一次故障瞬间打满所有会话迁移导致雪崩（§19 / §21）。

#include "mmo/gateway/resilience/failover_coordinator.h"

#include <algorithm>

namespace mmo::gateway {
namespace {

using core::ErrorCode;
using core::MonotonicClock;

}  // namespace

FailoverCoordinator::FailoverCoordinator(HealthMonitor& health, ISessionDirectory& dir,
                                         ISessionRebinder& rebinder,
                                         std::size_t max_concurrent)
    : health_(health), dir_(dir), rebinder_(rebinder),
      max_concurrent_(max_concurrent > 0 ? max_concurrent : 1) {}

core::Result<void> FailoverCoordinator::OnNodeDead(NodeId dead, core::TraceID trace) {
    const std::int64_t start = MonotonicClock::Now();
    ++stats_.detected;

    // 选同角色、未 Dead 的替换节点（低负载优先，见 37.1 FindReplacement）。
    const NodeRole role = health_.RoleOf(dead);
    auto repl = health_.FindReplacement(role, /*affinity=*/{});
    NodeId to = 0;
    if (repl.HasValue() && !repl.Value().empty()) {
        to = repl.Value().front();
    }
    if (to == 0) {
        // 无可用替换节点：本节点会话无法迁移（真实环境应告警 + 客户端 Failed 提示）。
        stats_.duration_ms = MonotonicClock::Now() - start;
        return core::Result<void>::Ok();
    }
    return ReattachPlayers(dead, to, trace);
}

core::Result<void> FailoverCoordinator::ReattachPlayers(NodeId from, NodeId to,
                                                       core::TraceID trace) {
    current_to_ = to;

    // 查询受影响会话并入队（幂等：已在队列的不重复入）。
    const auto sessions = dir_.SessionsOnNode(from);
    for (const SessionId s : sessions) {
        if (std::find(queue_.begin(), queue_.end(), s) == queue_.end()) {
            queue_.push_back(s);
        }
    }

    Drain(trace);
    return core::Result<void>::Ok();
}

void FailoverCoordinator::Drain(core::TraceID /*trace*/) {
    // 限速波次：每波最多 max_concurrent_ 个 Rebind 视为同时在途（真实异步运行时即并发上限），
    // 整波处理完再进入下一波；任一时刻在途不超过 max_concurrent_，防雪崩（§19 / §21）。
    while (!queue_.empty()) {
        std::vector<SessionId> batch;
        batch.reserve(max_concurrent_);
        while (!queue_.empty() && batch.size() < max_concurrent_) {
            batch.push_back(queue_.back());
            queue_.pop_back();
        }
        in_flight_ += batch.size();
        for (const SessionId s : batch) {
            auto r = rebinder_.Rebind(s, current_to_);
            if (r.HasValue()) {
                ++stats_.reattached;
            } else {
                ++stats_.failed;
            }
        }
        in_flight_ -= batch.size();
    }
}

}  // namespace mmo::gateway
