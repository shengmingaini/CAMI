// server/gateway/src/route/node_registry.cpp — TASK-010 §15.1 / §15.6
//
// 节点注册中心：注册 / 心跳 / 健康状态机 / 一致性哈希 Pick / Tick 剔除。

#include "mmo/gateway/route/node_registry.h"

#include <algorithm>

namespace mmo::gateway {
namespace {

using core::ErrorCode;
using core::MonotonicClock;

core::Error MakeErr(ErrorCode code, const char* msg) {
    return core::Error{code, msg, core::domain::kNet};
}

// --- FNV-1a 64 对 string_view 做哈希（affinity_key / PlayerId / SceneId 经字符串化后入参） ---
std::uint64_t HashKey(std::string_view key) noexcept {
    std::uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    for (const char c : key) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}

// Google "jump consistent hash"：O(log n)，输出稳定且近均匀；节点变动仅 ~1/n 重映射。
// 关键：b / j 必须为 int64 整数（与规范一致），每次迭代把 (b+1)*frac 截断回整数；
// 若用 double 累积会令桶边界漂移，导致分布系统性偏斜（实测 bucket 1 偏高 ~19%）。
std::size_t JumpConsistentHash(std::uint64_t key, std::size_t num_buckets) {
    if (num_buckets <= 1) {
        return 0;
    }
    const double kScale = static_cast<double>(1LL << 31);
    std::int64_t b = -1, j = 0;
    while (j < static_cast<std::int64_t>(num_buckets)) {
        b = j;
        key = key * 2862933555777941757ULL + 1;
        const double frac = kScale /
            static_cast<double>(static_cast<std::uint32_t>(key >> 33) + 1);
        j = static_cast<std::int64_t>(static_cast<double>(b + 1) * frac);
    }
    return static_cast<std::size_t>(b);
}

}  // namespace

const char* ToString(NodeRole role) noexcept {
    switch (role) {
        case NodeRole::GameNode:  return "GameNode";
        case NodeRole::SceneNode: return "SceneNode";
        default:                   return "Unknown";
    }
}

const char* ToString(NodeHealth health) noexcept {
    switch (health) {
        case NodeHealth::Healthy: return "Healthy";
        case NodeHealth::Suspect: return "Suspect";
        case NodeHealth::Dead:    return "Dead";
        default:                   return "Unknown";
    }
}

core::Result<void> NodeRegistry::Register(const NodeInfo& info) {
    if (info.id == 0) {
        return core::Result<void>::Fail(MakeErr(ErrorCode::INVALID_ARGUMENT, "invalid node id"));
    }
    for (auto& n : nodes_) {
        if (n.id == info.id) {
            // 更新元数据，保留现有健康 / 心跳状态
            n.addr           = info.addr;
            n.port           = info.port;
            n.role           = info.role;
            return core::Result<void>::Ok();
        }
    }
    NodeInfo copy = info;
    copy.last_heartbeat = MonotonicClock::Point();
    copy.health         = NodeHealth::Healthy;
    copy.missed         = 0;
    nodes_.push_back(std::move(copy));
    return core::Result<void>::Ok();
}

core::Result<void> NodeRegistry::Heartbeat(NodeId id, std::uint32_t load) {
    for (auto& n : nodes_) {
        if (n.id == id) {
            n.last_heartbeat = MonotonicClock::Point();
            n.load           = load;
            n.missed         = 0;
            if (n.health == NodeHealth::Suspect) {
                n.health = NodeHealth::Healthy;  // 恢复
            }
            return core::Result<void>::Ok();
        }
    }
    return core::Result<void>::Fail(MakeErr(ErrorCode::NOT_FOUND, "node not registered"));
}

core::Result<void> NodeRegistry::Unregister(NodeId id) {
    const auto before = nodes_.size();
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                [id](const NodeInfo& n) { return n.id == id; }),
                 nodes_.end());
    if (nodes_.size() == before) {
        return core::Result<void>::Fail(MakeErr(ErrorCode::NOT_FOUND, "node not registered"));
    }
    return core::Result<void>::Ok();
}

core::Result<std::vector<NodeInfo>> NodeRegistry::AliveFor(NodeRole role) const {
    std::vector<NodeInfo> out;
    for (const auto& n : nodes_) {
        if (n.role == role && n.health != NodeHealth::Dead) {
            out.push_back(n);
        }
    }
    return core::Result<std::vector<NodeInfo>>::Ok(std::move(out));
}

core::Result<std::vector<NodeInfo>> NodeRegistry::ListHealthy(NodeRole role) const {
    return AliveFor(role);
}

core::Result<NodeId> NodeRegistry::Pick(NodeRole role, std::string_view affinity_key) {
    auto alive = AliveFor(role);
    if (!alive.HasValue()) {
        return core::Result<NodeId>::Fail(alive.Err());
    }
    const auto& list = alive.Value();
    if (list.empty()) {
        // §19 / §20.5：全部节点不可用，明确 BUSY，不静默崩溃
        return core::Result<NodeId>::Fail(MakeErr(ErrorCode::BUSY, "no alive node for role"));
    }
    if (list.size() == 1) {
        return core::Result<NodeId>::Ok(list[0].id);
    }

    const std::uint64_t h = HashKey(affinity_key);
    // 一致性哈希主选：稳定 + 近均匀
    std::size_t idx = JumpConsistentHash(h, list.size());

    // §15.7 负载感知：在主选及其后 K 个邻节点（环形）中优先低负载节点。
    // 负载相等时回退到主选（保证一致性哈希测试不受扰动）。
    constexpr std::size_t kWindow = 2;
    std::size_t best              = idx;
    for (std::size_t k = 1; k <= kWindow; ++k) {
        const std::size_t j = (idx + k) % list.size();
        if (list[j].load < list[best].load) {
            best = j;
        }
    }
    return core::Result<NodeId>::Ok(list[best].id);
}

std::size_t NodeRegistry::HealthyCount(NodeRole role) const noexcept {
    std::size_t c = 0;
    for (const auto& n : nodes_) {
        if (n.role == role && n.health != NodeHealth::Dead) {
            ++c;
        }
    }
    return c;
}

core::Result<void> NodeRegistry::Tick(core::SteadyTime now) {
    const std::int64_t interval_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config_.heartbeat_interval)
            .count();
    if (interval_ns <= 0) {
        return core::Result<void>::Ok();
    }

    std::vector<NodeId> dead;
    for (auto& n : nodes_) {
        if (n.health == NodeHealth::Dead) {
            continue;  // 已被标记，等待下方统一剔除
        }
        const std::int64_t elapsed =
            static_cast<std::int64_t>(MonotonicClock::Elapsed(n.last_heartbeat, now));
        const std::uint32_t missed =
            static_cast<std::uint32_t>(elapsed / interval_ns);
        if (missed == n.missed) {
            continue;  // 同一心跳窗内，无需重复处理
        }
        n.missed = missed;
        if (missed >= kDeadAfterMisses) {
            n.health = NodeHealth::Dead;
            dead.push_back(n.id);
        } else if (missed >= kSuspectAfterMisses) {
            n.health = NodeHealth::Suspect;
        }
    }

    // 判定 Dead：发布 NodeDead 事件并剔除，停止后续路由
    for (const NodeId id : dead) {
        if (bus_ != nullptr) {
            const auto it = std::find_if(nodes_.begin(), nodes_.end(),
                                         [id](const NodeInfo& n) { return n.id == id; });
            if (it != nodes_.end()) {
                (void)bus_->Publish(NodeDead{id, it->role, now});
            }
        }
        nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                    [id](const NodeInfo& n) { return n.id == id; }),
                     nodes_.end());
    }
    return core::Result<void>::Ok();
}

}  // namespace mmo::gateway
