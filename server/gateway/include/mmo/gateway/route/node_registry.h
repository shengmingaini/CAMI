// server/gateway/include/mmo/gateway/route/node_registry.h — TASK-010 §7
//
// 节点注册中心 + 健康状态机 + 一致性哈希 Pick。
// 红线（§21 / §27.3）：本模块归 server/gateway 所有；禁止全局锁保护路由表
// （§9：分片 / 单写者，不引入全局锁）；禁止在 Tick 热路径做阻塞 IO。
//
// 依赖：server/gateway/session 提供 NodeId（§7 Pick 返回 NodeId）。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/session/session.h"

namespace mmo::gateway {

enum class NodeRole : std::uint8_t {
    GameNode  = 0,  // 实时游戏逻辑节点
    SceneNode = 1,  // 场景承载节点（与 GameNode 同进程时复用，逻辑上区分）
    Unknown   = 0xFF,
};

/// 节点健康三态机（§8 / §19）：Heartbeat 丢失次数驱动迁移。
enum class NodeHealth : std::uint8_t {
    Healthy = 0,  // 正常
    Suspect = 1,  // 1 次心跳丢失（仍可路由，谨慎）
    Dead    = 2,  // 3 次丢失（剔除，停止路由，发布 NodeDead）
};

const char* ToString(NodeRole role) noexcept;
const char* ToString(NodeHealth health) noexcept;

inline constexpr std::uint32_t kSuspectAfterMisses = 1;  // 丢 1 次 → Suspect
inline constexpr std::uint32_t kDeadAfterMisses    = 3;  // 丢 3 次 → Dead

// Config 定义在命名空间级（非类内嵌套）：GCC 下「嵌套 struct 带 NSDMI + 作默认实参
// （Config config = {}）」会编译失败（TASK-007 NodeRegistry / TASK-009 SessionManager 同坑）。
// 类内用 using 保持调用方写法不变。
struct NodeRegistryConfig {
    core::DurationMs heartbeat_interval{5000};  // 期望心跳间隔
};

struct NodeInfo {
    NodeId          id{0};
    std::string     addr;
    std::uint16_t   port{0};
    NodeRole        role{NodeRole::GameNode};
    std::uint32_t   load{0};               // 来自 GameNode 心跳上报的负载
    core::SteadyTime last_heartbeat{};     // 最近一次心跳（单调时钟）
    NodeHealth      health{NodeHealth::Healthy};
    std::uint32_t   missed{0};             // 连续丢失心跳次数
};

/// GameNode 被判定 Dead 时发布（供 TASK-037 故障迁移消费）。纯 POD 事件。
struct NodeDead {
    NodeId        node_id{0};
    NodeRole      role{NodeRole::GameNode};
    core::SteadyTime at{};
};

class NodeRegistry {
public:
    using Config = NodeRegistryConfig;

    explicit NodeRegistry(core::EventBus* bus = nullptr)
        : bus_(bus), config_() {}
    NodeRegistry(core::EventBus* bus, NodeRegistryConfig config)
        : bus_(bus), config_(std::move(config)) {}

    /// 注册 / 更新节点。重复 Register 视为更新元数据（不影响健康状态）。
    core::Result<void> Register(const NodeInfo& info);

    /// 心跳上报：刷新 last_heartbeat + load，missed 清零，Suspect→Healthy 恢复。
    core::Result<void> Heartbeat(NodeId id, std::uint32_t load);

    /// 主动注销。
    core::Result<void> Unregister(NodeId id);

    /// 列出某角色的存活节点（Health != Dead）。
    core::Result<std::vector<NodeInfo>> ListHealthy(NodeRole role) const;

    /// 一致性哈希选节点（§8 / §16）：同 key 稳定命中同一节点；节点变动时仅 ~1/n 重映射。
    /// 全部不可用时返回 BUSY（§19 / §20.5）。
    core::Result<NodeId> Pick(NodeRole role, std::string_view affinity_key = {});

    /// 心跳超时扫描（§9 由独立定时器驱动，不在转发路径）：Suspect/Dead 状态迁移；
    /// 判定 Dead 时发布 NodeDead 事件并剔除。
    core::Result<void> Tick(core::SteadyTime now);

    /// 存活节点计数（Health != Dead）。
    std::size_t HealthyCount(NodeRole role) const noexcept;

    /// 当前注册节点总数（含 Dead，供测试 / 观测）。
    std::size_t Size() const noexcept { return nodes_.size(); }

private:
    core::Result<std::vector<NodeInfo>> AliveFor(NodeRole role) const;

    core::EventBus* bus_;
    NodeRegistryConfig config_;
    // 单写者（Gateway NetworkThread 独占），无锁。跨线程只读查询走快照（此处略，测试单线程）。
    std::vector<NodeInfo> nodes_;
};

}  // namespace mmo::gateway
