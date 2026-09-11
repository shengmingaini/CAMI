// server/control/include/mmo/control/control_service.h — TASK-040 §7
//
// ControlService：集群控制面（节点管理 / 配置下发 / 健康 / 运维）。
//
// 红线（§21 / §27.3）：
//   - 不拥有任何游戏实时状态（玩家 / Scene / Entity 等），只做控制与协调。
//   - 公开头禁止 include 内部 src/；下游只能通过本头调用。
//   - 配置变更必须带 config_version 且可回退（回退 = 重新 push 旧快照 + 更大版本号）。
//   - 灰度发布 / 自动扩缩容列为 Phase 2 RFC，本版不实现。
//
// 依赖（§27.2，仅消费上游公开接口）：
//   - TASK-003  engine/core ：EventBus / 时钟 / ConfigManager（热加载原子替换思想）。
//   - TASK-006  engine/rpc  ：gRPC 传输（见 control_service.proto，本头为进程内实现）。
//   - TASK-010  server/gateway ：节点离线时发布其 NodeDead 触发路由缓存失效协调。

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/time/clock.h"
#include "mmo/data/idata_store.h"

namespace mmo::control {

/// 节点角色（控制面视角；Gateway 多实例也在此登记，供前置 LB 分流）。
enum class ControlRole : std::uint8_t {
    GameNode  = 0,
    Gateway   = 1,
    SceneNode = 2,
    Unknown   = 0xFF,
};

/// 节点在线状态（由控制面权威判定，单写者模型）。
enum class NodeStatus : std::uint8_t {
    Online  = 0,
    Offline = 1,
    Unknown = 0xFF,
};

/// 控制面节点记录。
/// 注意：只含控制 / 协调元数据，**绝不**含游戏实时状态（玩家 / Scene / Entity）。
/// node_id 复用 TASK-010 的 NodeId 数值空间（uint32），使离线协调可直接发布其 NodeDead。
struct ControlNodeInfo {
    std::uint32_t   node_id{0};
    ControlRole     role{ControlRole::GameNode};
    std::string     addr;
    std::uint32_t   capacity{0};        // 前置 LB 分流：容量权重
    std::uint32_t   load{0};            // 当前负载（心跳上报）
    std::uint32_t   player_count{0};    // 心跳上报
    std::uint32_t   tick_p99_ms{0};     // 心跳上报（p99 tick 延迟）
    NodeStatus      status{NodeStatus::Online};
    core::SteadyTime last_heartbeat{};
};

/// 配置快照：不可变载体，整体原子替换（复用 TASK-003 原子替换思想，读侧无锁）。
struct ConfigPayload {
    std::uint64_t version{0};
    std::string   snapshot;
};

/// 节点上线（注册 / 重新注册）。
struct NodeRegistered {
    std::uint32_t node_id{0};
    ControlRole   role{ControlRole::GameNode};
};

/// 心跳到达（含负载指标）。
struct NodeHeartbeat {
    std::uint32_t node_id{0};
    std::uint32_t load{0};
    std::uint32_t player_count{0};
};

/// 节点被控制面权威判定离线（控制面自身事件，供监控 / 运维消费）。
struct NodeOffline {
    std::uint32_t node_id{0};
    ControlRole   role{ControlRole::GameNode};
};

/// 配置已下发且版本推进（节点据此热加载）。
struct ConfigPushed {
    std::uint64_t version{0};
};

/// ControlService 构造选项。
/// 注意：必须定义在命名空间作用域（而非 ControlService 内部嵌套），否则「嵌套 struct 带
/// NSDMI + 作默认实参」会在 GCC 下编译失败（TASK-007 NodeRegistry / TASK-009 同坑）。
struct Options {
    /// 超过该间隔未收到心跳 → 判离线（默认 15s，显著大于网关 5s 心跳以容忍抖动）。
    core::DurationMs heartbeat_timeout = core::DurationMs(15000);
    /// 注册未给出 capacity 时的兜底值。
    std::uint32_t   default_capacity = 1000;
};

/// 集群控制面。
///
/// 线程模型（§9）：节点表由控制线程单写者维护（std::mutex 保护写，读侧走快照拷贝）；
/// 配置快照用 std::atomic<shared_ptr> 整体替换，读侧无锁、无分配。
class ControlService {
public:
    /// store 可为空：为空时纯内存（不持久化），用于控制面无 DB 启动 / 测试。
    /// 非空时经 TASK-026 IDataStore 接口持久化（禁止直连 MySQL / Redis）。
    explicit ControlService(core::EventBus& bus,
                           mmo::data::IDataStore* store = nullptr,
                           Options opts = {});

    ControlService(const ControlService&) = delete;
    ControlService& operator=(const ControlService&) = delete;

    // ---- 节点管理（权威注册表，§15.2 / §15.6）----
    core::Result<void> RegisterNode(std::uint32_t node_id, ControlRole role,
                                    std::string_view addr,
                                    std::uint32_t capacity = 0);

    /// 心跳：刷新负载 + last_heartbeat；离线节点心跳可恢复为 Online。
    core::Result<void> Heartbeat(std::uint32_t node_id, std::uint32_t load,
                                 std::uint32_t player_count = 0,
                                 std::uint32_t tick_p99_ms = 0);

    /// 主动注销（优雅下线）：触发路由失效协调，等同离线。
    core::Result<void> UnregisterNode(std::uint32_t node_id);

    // ---- 配置下发（§15.3）----
    /// version 必须严格大于当前版本（单调递增；回退请用「重新 push 旧快照 + 更大版本号」）。
    core::Result<void> PushConfig(std::uint64_t version, std::string snapshot);
    std::uint64_t ConfigVersion() const noexcept;

    /// 无锁读取当前配置快照（读侧拿 shared_ptr<const>，复用 TASK-003 原子替换，不影响在途读者）。
    std::shared_ptr<const ConfigPayload> CurrentConfig() const noexcept;

    // ---- 拓扑查询（运维，§15.4）----
    std::vector<ControlNodeInfo> QueryTopology() const;
    std::vector<ControlNodeInfo> OnlineNodes(ControlRole role) const;
    std::optional<ControlNodeInfo> FindNode(std::uint32_t node_id) const;
    std::size_t OnlineCount(ControlRole role) const;

    /// Gateway 多实例：返回所有 Gateway 的 capacity/load，供前置 LB 分流决策（§15.6）。
    std::vector<ControlNodeInfo> GatewayInstances() const;

    // ---- 心跳超时扫描（由控制线程定时器驱动，绝不在游戏 Tick 热路径，§9）----
    core::Result<void> Tick(core::SteadyTime now);

    /// 降级标记：ControlService 崩溃后游戏节点用本地缓存继续服务，不雪崩（§19）。
    /// 本服务不持有游戏状态，节点本地缓存拓扑 / 路由；崩溃不影响在线服务，仅停止协调。
    bool Degraded() const noexcept { return degraded_; }

private:
    core::Result<void> PersistNodes();
    core::Result<void> PersistConfig();
    void               RestoreIfStorePresent();
    void               MarkOffline(ControlNodeInfo& node, core::SteadyTime now);

    core::EventBus&        bus_;
    mmo::data::IDataStore* store_;
    Options                opts_;

    mutable std::mutex                    mu_;
    std::vector<ControlNodeInfo>         nodes_;                 // 单写者
    std::atomic<std::shared_ptr<const ConfigPayload>> config_;   // 原子替换，无锁读
    bool                                 degraded_{false};
};

}  // namespace mmo::control
