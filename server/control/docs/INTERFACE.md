# ControlService · INTERFACE

> TASK-040 · 控制面：节点管理 / 配置下发 / 健康 / 运维。
> 本文件是导出接口与冻结契约的权威说明，下游依赖方以此为准。

## 1. 定位与边界（§4 / §21 / §27.3）

- ControlService 是集群控制面：**节点注册表权威方**、**配置版本下发方**、**健康检查/运维接口提供方**、**Gateway 多实例注册方**。
- **不拥有任何游戏实时状态**（玩家 / Scene / Entity）。`ControlNodeInfo` 只含控制/协调元数据。
- 灰度发布、自动扩缩容列为 **Phase 2 RFC**，本版不实现（不塞进第一版）。
- 禁止：在 GameNode 内硬编码节点表（必须来自 ControlService 下发）；配置变更破坏旧版本兼容；循环依赖。

## 2. 导出接口（`server/control/include/mmo/control/control_service.h`）

### 类型
| 类型 | 说明 |
|---|---|
| `enum class ControlRole : uint8_t` | `GameNode` / `Gateway` / `SceneNode` / `Unknown` |
| `enum class NodeStatus : uint8_t` | `Online` / `Offline` / `Unknown` |
| `struct ControlNodeInfo` | `node_id(uint32)`, `role`, `addr`, `capacity`, `load`, `player_count`, `tick_p99_ms`, `status`, `last_heartbeat` |
| `struct ConfigPayload` | `version(uint64)`, `snapshot(string)`（不可变快照） |
| 事件 `NodeRegistered` / `NodeHeartbeat` / `NodeOffline` / `ConfigPushed` | 发布到共享 EventBus，供监控/运维消费 |

### `class ControlService`
| 方法 | 语义 |
|---|---|
| `RegisterNode(node_id, role, addr, capacity=0)` | 注册/更新节点；`capacity==0` 用默认兜底；重复注册视为更新元数据 |
| `Heartbeat(node_id, load, player_count=0, tick_p99_ms=0)` | 刷新负载与 `last_heartbeat`；离线节点心跳可恢复 Online |
| `UnregisterNode(node_id)` | 优雅下线：触发路由失效协调（等同离线） |
| `PushConfig(version, snapshot)` | 配置下发；`version` 必须严格大于当前（单调）；返回旧版本即拒绝 |
| `ConfigVersion()` | 当前配置版本号 |
| `CurrentConfig()` | **无锁**读取当前快照（`shared_ptr<const>`，原子替换，不影响在途读者） |
| `QueryTopology()` / `OnlineNodes(role)` / `FindNode(id)` / `OnlineCount(role)` | 拓扑查询 |
| `GatewayInstances()` | 返回在线 Gateway 的 `capacity/load`，供前置 LB 分流 |
| `Tick(now)` | 心跳超时扫描：超时节点判离线（单写者，非热路径） |
| `Degraded()` | 降级标记（控制面崩溃后游戏节点用本地缓存继续服务，不雪崩） |

**扩展性**：节点角色、事件均为注册式/枚举式，**无 switch 硬编码社交/节点类型**；新增角色只需扩 `ControlRole` 枚举。

## 3. 消费的上游接口（§27.2，仅公开头，禁止 include src/）

| 上游任务 | 消费内容 | 用途 |
|---|---|---|
| **TASK-003** `engine/core` | `core::EventBus` / `core::MonotonicClock` / `core::SteadyTime` / `core::DurationMs`；`ConfigManager` 原子快照替换思想 | 事件发布、时钟、配置无锁热加载（复用 TASK-003 原子替换） |
| **TASK-006** `engine/rpc` | gRPC 统一封装；传输契约见 `protocol/proto/service/control_service.proto` | 跨进程 RPC（本版提供 .proto 契约；进程内 API 为已验证实现） |
| **TASK-010** `server/gateway` | `mmo::gateway::NodeDead`（`node_registry.h` 公开事件） | **节点离线 → 路由缓存失效协调**：ControlService 判离线后发布 `NodeDead`，`GatewayRouter` 已订阅并批量失效其路由缓存 |

> 依赖方向：`server/control -> engine/core` + `server/dataservice`（单向，无环）。与 TASK-010 的协调通过共享 EventBus 的 `NodeDead` 事件完成，**不在 ControlService 内直接操作 Gateway 路由表**。

## 4. 持久化约定（§13，经 `mmo::data::IDataStore`）

- 键：节点表聚合 `control:nodes`、配置 `control:config`。
- 序列化：`server/control/src/control_io.{h,cpp}`（内部，未进 `include/`）。字段分隔 `\x1f`，记录分隔 `\n`；snapshot 用 `version \x1f len \x1f <bytes>` 防嵌入分隔符。
- 写入用 `VersionCheck{required=false}`（简单 KV 覆盖）；`store==nullptr` 时纯内存（无 DB 启动/测试）。
- **禁止直连 MySQL / Redis**：持久化一律经 `IDataStore` 接口。

## 5. 配置

- `config/control/control.json`：`heartbeat_timeout_ms`(默认 15000)、`default_capacity`(默认 1000)。
- 运行时由 `ControlService::Options` 注入；可从 TASK-003 `ConfigManager` 读取覆写。

## 6. 测试与验收（§16 / §17 / §19 / §20）

- 测试二进制 `control_test` → `ctest -R Control`（注册名 `Game_Control.Suite`）。
- 覆盖：
  1. 节点注册/心跳/超时/拓扑查询（§16）；
  2. 配置版本比对 + 原子热加载（旧读者不受影响，§20 #3）；
  3. **路由失效协调**：ControlService 离线 → 发布 `NodeDead` → `GatewayRouter` 路由缓存失效（接 TASK-010，§20 #2）；
  4. Gateway 多实例 `capacity/load` 输出（§20 #4）；
  5. **崩溃降级**：ControlService 销毁后节点本地缓存仍可用，不雪崩（§20 #5）；
  6. 可选持久化：FakeStore 注入 → 重启恢复节点表与配置。
- 构建：`Debug` + `Release` 双构建 **0 警告**（含 `-Wconversion -Wsign-conversion -Wold-style-cast`）。

## 7. Phase 2 边界（明确不在本版）

- 灰度发布（按节点/区域渐进推送配置）；自动扩缩容（依据 `capacity/load` 触发 Gateway 增减）。
- 上述能力需新增 RFC，不破坏本版接口契约。
