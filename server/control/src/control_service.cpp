// server/control/src/control_service.cpp — TASK-040 控制面核心实现
//
// 设计要点：
//   - 节点表单写者（控制线程），std::mutex 保护写，读侧走快照拷贝。
//   - 配置快照用 std::atomic<shared_ptr<const ConfigPayload>> 整体替换（复用 TASK-003
//     原子替换思想），读侧无锁、无分配，热加载不中断在途读者。
//   - 节点离线（心跳超时 / 主动注销）时：发布控制面 NodeOffline + 复用 TASK-010 的
//     NodeDead（同一 EventBus），Gateway 的 GatewayRouter 已订阅 NodeDead → 路由缓存失效。
//   - 持久化（可选）经 mmo::data::IDataStore 接口，禁止直连 MySQL / Redis。

#include "mmo/control/control_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "control_io.h"
// 复用 TASK-010 的 NodeDead 作为跨模块「节点失效」协调信号（消费其公开头，未 include src/）。
#include "mmo/gateway/route/node_registry.h"

namespace mmo::control {

namespace {

constexpr std::string_view kDomain = "control";
constexpr std::string_view kNodesKey = "control:nodes";
constexpr std::string_view kConfigKey = "control:config";

inline core::Error CtlErr(core::ErrorCode code, std::string_view msg) {
    return core::Error(code, msg, kDomain);
}

inline mmo::gateway::NodeRole ToGatewayRole(ControlRole r) {
    switch (r) {
        case ControlRole::SceneNode:
            return mmo::gateway::NodeRole::SceneNode;
        default:
            return mmo::gateway::NodeRole::GameNode;
    }
}

inline mmo::gateway::NodeId ToGatewayNodeId(std::uint32_t id) {
    return static_cast<mmo::gateway::NodeId>(id);
}

}  // namespace

ControlService::ControlService(core::EventBus& bus, mmo::data::IDataStore* store, Options opts)
    : bus_(bus),
      store_(store),
      opts_(opts),
      config_(std::make_shared<const ConfigPayload>()) {
    RestoreIfStorePresent();
}

core::Result<void> ControlService::RegisterNode(std::uint32_t node_id, ControlRole role,
                                                std::string_view addr, std::uint32_t capacity) {
    if (addr.empty()) {
        return core::Result<void>::Fail(CtlErr(core::ErrorCode::INVALID_ARGUMENT, "empty addr"));
    }
    if (capacity == 0) capacity = opts_.default_capacity;

    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [node_id](const ControlNodeInfo& n) { return n.node_id == node_id; });
    ControlNodeInfo info;
    info.node_id = node_id;
    info.role = role;
    info.addr = std::string(addr);
    info.capacity = capacity;
    info.status = NodeStatus::Online;
    info.last_heartbeat = core::MonotonicClock::Point();
    if (it != nodes_.end()) {
        *it = std::move(info);  // 重复注册视为更新元数据，不影响在线状态
    } else {
        nodes_.push_back(std::move(info));
    }
    (void)bus_.Publish(NodeRegistered{node_id, role});
    (void)PersistNodes();
    return core::Result<void>::Ok();
}

core::Result<void> ControlService::Heartbeat(std::uint32_t node_id, std::uint32_t load,
                                             std::uint32_t player_count, std::uint32_t tick_p99_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [node_id](const ControlNodeInfo& n) { return n.node_id == node_id; });
    if (it == nodes_.end()) {
        return core::Result<void>::Fail(CtlErr(core::ErrorCode::NOT_FOUND, "unknown node"));
    }
    it->load = load;
    it->player_count = player_count;
    it->tick_p99_ms = tick_p99_ms;
    it->last_heartbeat = core::MonotonicClock::Point();
    if (it->status == NodeStatus::Offline) {
        it->status = NodeStatus::Online;  // 离线节点心跳可恢复
    }
    (void)bus_.Publish(NodeHeartbeat{node_id, load, player_count});
    (void)PersistNodes();
    return core::Result<void>::Ok();
}

core::Result<void> ControlService::UnregisterNode(std::uint32_t node_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [node_id](const ControlNodeInfo& n) { return n.node_id == node_id; });
    if (it == nodes_.end()) {
        return core::Result<void>::Fail(CtlErr(core::ErrorCode::NOT_FOUND, "unknown node"));
    }
    const core::SteadyTime now = core::MonotonicClock::Point();
    MarkOffline(*it, now);
    nodes_.erase(it);
    (void)PersistNodes();
    return core::Result<void>::Ok();
}

core::Result<void> ControlService::PushConfig(std::uint64_t version, std::string snapshot) {
    auto cur = config_.load(std::memory_order_acquire);
    if (version <= cur->version) {
        return core::Result<void>::Fail(
            CtlErr(core::ErrorCode::INVALID_ARGUMENT, "stale config version (must be monotonic)"));
    }
    auto next = std::make_shared<ConfigPayload>();
    next->version = version;
    next->snapshot = std::move(snapshot);
    config_.store(next, std::memory_order_release);  // 原子整体替换：读侧无锁、无中断
    (void)bus_.Publish(ConfigPushed{version});
    (void)PersistConfig();
    return core::Result<void>::Ok();
}

std::uint64_t ControlService::ConfigVersion() const noexcept {
    return config_.load(std::memory_order_acquire)->version;
}

std::shared_ptr<const ConfigPayload> ControlService::CurrentConfig() const noexcept {
    return config_.load(std::memory_order_acquire);  // 返回 shared_ptr 副本（无锁）
}

std::vector<ControlNodeInfo> ControlService::QueryTopology() const {
    std::lock_guard<std::mutex> lock(mu_);
    return nodes_;
}

std::vector<ControlNodeInfo> ControlService::OnlineNodes(ControlRole role) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ControlNodeInfo> out;
    for (const auto& n : nodes_) {
        if (n.role == role && n.status == NodeStatus::Online) out.push_back(n);
    }
    return out;
}

std::optional<ControlNodeInfo> ControlService::FindNode(std::uint32_t node_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [node_id](const ControlNodeInfo& n) { return n.node_id == node_id; });
    if (it == nodes_.end()) return std::nullopt;
    return *it;
}

std::size_t ControlService::OnlineCount(ControlRole role) const {
    return OnlineNodes(role).size();
}

std::vector<ControlNodeInfo> ControlService::GatewayInstances() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ControlNodeInfo> out;
    for (const auto& n : nodes_) {
        if (n.role == ControlRole::Gateway && n.status == NodeStatus::Online) out.push_back(n);
    }
    return out;
}

core::Result<void> ControlService::Tick(core::SteadyTime now) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::int64_t timeout_ns =
        static_cast<std::int64_t>(opts_.heartbeat_timeout.count()) * core::kSteadyNsPerMilli;
    bool changed = false;
    for (auto& n : nodes_) {
        if (n.status == NodeStatus::Offline) continue;
        const std::int64_t elapsed_ns = core::MonotonicClock::Elapsed(n.last_heartbeat, now);
        if (elapsed_ns >= timeout_ns) {
            MarkOffline(n, now);
            changed = true;
        }
    }
    if (changed) (void)PersistNodes();
    return core::Result<void>::Ok();
}

void ControlService::MarkOffline(ControlNodeInfo& node, core::SteadyTime now) {
    node.status = NodeStatus::Offline;
    // 控制面事件：供监控 / 运维消费。
    (void)bus_.Publish(NodeOffline{node.node_id, node.role});
    // 跨模块协调：复用 TASK-010 NodeDead，Gateway 的 GatewayRouter 订阅后批量失效路由缓存。
    (void)bus_.Publish(mmo::gateway::NodeDead{ToGatewayNodeId(node.node_id),
                                              ToGatewayRole(node.role), now});
}

core::Result<void> ControlService::PersistNodes() {
    if (store_ == nullptr) return core::Result<void>::Ok();
    const std::string payload = detail::SerializeNodes(nodes_);
    mmo::data::Record rec;
    rec.key = std::string(kNodesKey);
    rec.payload = payload;
    rec.updated_at = core::MonotonicClock::Point();
    mmo::data::VersionCheck vc;
    vc.required = false;  // 控制面节点表：简单 KV 覆盖，不做乐观锁
    return store_->Save(rec, vc);
}

core::Result<void> ControlService::PersistConfig() {
    if (store_ == nullptr) return core::Result<void>::Ok();
    auto cur = config_.load(std::memory_order_acquire);
    const std::string payload = detail::SerializeConfig(cur->version, cur->snapshot);
    mmo::data::Record rec;
    rec.key = std::string(kConfigKey);
    rec.payload = payload;
    rec.updated_at = core::MonotonicClock::Point();
    mmo::data::VersionCheck vc;
    vc.required = false;
    return store_->Save(rec, vc);
}

void ControlService::RestoreIfStorePresent() {
    if (store_ == nullptr) return;
    auto loaded = store_->Load(std::string(kNodesKey));
    if (loaded.HasValue() && loaded.Value().has_value()) {
        auto nodes = detail::DeserializeNodes(loaded.Value()->payload);
        if (nodes.HasValue()) {
            std::lock_guard<std::mutex> lock(mu_);
            nodes_ = std::move(nodes.Value());
        }
    }
    auto cfg = store_->Load(std::string(kConfigKey));
    if (cfg.HasValue() && cfg.Value().has_value()) {
        auto parsed = detail::DeserializeConfig(cfg.Value()->payload);
        if (parsed.HasValue()) {
            auto next = std::make_shared<ConfigPayload>();
            next->version = parsed.Value().first;
            next->snapshot = std::move(parsed.Value().second);
            config_.store(next, std::memory_order_release);
        }
    }
}

}  // namespace mmo::control
