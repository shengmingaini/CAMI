# 模块设计文档：心跳管理（Heartbeat Manager）

> 层级：接入层（Gateway） · 子模块：心跳机制与超时踢线、空闲连接回收
> 状态标签：`[PROTOTYPE]`（管理器级心跳地基；与真实客户端心跳帧的端到端联调见开放问题）
> 对应周计划：Week2 周三 `网关·连接管理 → 心跳机制与超时踢线、空闲连接回收 → 心跳模块`

---

## 1. 概述

心跳管理模块负责在**管理器级**对全部客户端连接做心跳检测、超时踢线与空闲回收。它是接入层"无状态水平扩展、无全局锁"架构下，连接活性治理的权威组件（架构 §4.1 接入层职责：*维持客户端连接、心跳检测、超时断开*）。

- **不拥有 socket、不绑定端口、不依赖 Asio**：纯 `std` 实现，零外部依赖 → `CAMI_BUILD_MODULES=OFF` 即可独立编译并进 CI（满足"模块独立编译"验收）。
- **时钟注入式**：所有方法以 `TimePoint now` 入参，调用方（或测试）完全控制时钟 → 确定性、可单测。
- **与周一 `Connection` 解耦**：不改动已绿的 `connection` 模块（scope 纪律）；`ConnectionManager` 通过文档约定的集成缝接入（见 §9）。

## 2. 职责边界（架构红线）

| 做 | 不做 |
|---|---|
| 跟踪每条连接的最后活动时刻 | 不拥有 socket / 不收发字节 |
| 周期性扫描，判超时 → 调踢线闭包 | 不解析协议、不碰编解码（归 `codec`） |
| 移除过期/注销条目（无泄漏） | 不持有 Player、不做业务语义 |
| 提供 `live_count()` 供容量监控 | 不做全服广播、不用全局锁 |

## 3. 对外接口（IDL / 函数签名）

```cpp
namespace cami::gateway::heartbeat {

enum class TimeoutReason { kHeartbeatLost, kIdleRecycled };

struct HeartbeatConfig {
    std::chrono::milliseconds heartbeat_timeout{30000};   // 无活动宽限 → 踢线
    std::chrono::milliseconds idle_recycle_timeout{0};    // 可选二次阈值（0=关）
    std::chrono::milliseconds scan_interval{500};         // 扫描周期；误差 ≤ 此值
};

using TimePoint = std::chrono::steady_clock::time_point;
using KickFn = std::function<void(TimeoutReason)>;

class HeartbeatManager {
    explicit HeartbeatManager(HeartbeatConfig cfg = {});
    void register_connection(std::uint64_t id, TimePoint now, KickFn kick); // 注册+绑踢线闭包
    void unregister(std::uint64_t id);                                       // 显式注销（无泄漏）
    void mark_activity(std::uint64_t id, TimePoint now);                    // 刷新活动（防误杀）
    std::size_t live_count() const;                                          // 当前 live 数
    struct TimedOut { std::uint64_t id; TimeoutReason reason; };
    std::vector<TimedOut> tick(TimePoint now);                               // 扫描→踢线+移除
};

}
```

## 4. 数据结构

`HeartbeatManager` 内部以 `std::unordered_map<uint64_t, Entry{ TimePoint last; KickFn kick; }> live_` 跟踪；`Entry.kick` 为调用方绑定的踢线闭包（捕获连接句柄），`tick` 判定超时后调用闭包并 `erase` 条目。

## 5. 并发模型

- 单一 `std::mutex` 守护 `live_`：`register / mark_activity / unregister / tick` 均加锁（冷/温路径）。
- `tick` 在锁内仅做"收集过期条目 + 移动出 kick 闭包 + erase"，**锁外**调用 kick 闭包，避免回调内重入（如连接 `close` 触发 `unregister`）导致递归死锁。
- 无全局锁、无同步阻塞 IO，契合架构红线。

## 6. 性能瓶颈预判

- `tick` 为 O(N) 全表扫描（N=live 连接数）。5 万长连接下每 `scan_interval`(500ms) 扫一次 → 20 次/秒 × 5 万 = 100 万次/秒简单比较，单线程可承受；但大 N 时可采用分片/时间轮优化（见开放问题）。
- `mark_activity` 每次加锁 → 热路径（每消息一次）有锁竞争。生产建议把 `last_activity` 下沉为 `Connection` 内的 `std::atomic<TimePoint>`，`tick` 只读，去掉此锁（§10）。

## 7. 依赖关系

```
heartbeat ──(纯 std)──> 无下层依赖
gateway ── PUBLIC link ──> cami_gateway_heartbeat
```
独立 STATIC target：`cami_gateway_heartbeat`（不链任何外部库）。

## 8. 测试与验收

| 验收项 | 证据 | 结果 |
|---|---|---|
| 模块独立编译 | `cami_gateway_heartbeat` 纯 std，OFF 构建进 CI | ✅ |
| 心跳误差 < 1s | `heartbeat_test.ErrorWithinOneSecond`：每 500ms 扫描，检测延迟 ≤ 500ms < 1s | ✅ |
| 回收无泄漏 | `heartbeat_test.UnregisterNoLeak` + `heartbeat_selfcheck`：`live_count` 归零 | ✅ |
| 边界/保活/空闲回收 | GTest 7 项 + selfcheck 4 组断言 | ✅ |

CI 双保险：`heartbeat_selfcheck()` 接入 `skeleton_layer_check`（CI 永远编译+功能验证）；GTest `cami_heartbeat_test` 本地覆盖边界（CI `CAMI_BUILD_TESTS=OFF` 默认不跑，本地沙箱已 3/3 绿）。

## 9. 集成缝（与 ConnectionManager）

本模块刻意**不改动**周一 `Connection/Manager`（保 CI 绿）。生产接入方式（伪代码）：

```cpp
// ConnectionManager 持有：
std::shared_ptr<HeartbeatManager> hb_ = std::make_shared<HeartbeatManager>(cfg);
// 启动一个 steady_timer，每 cfg.scan_interval 调：hb_->tick(steady_clock::now());

// do_accept 中，新连接注册（闭包捕获 shared_ptr<Connection> 保活并 close）：
auto conn = std::make_shared<Connection>(std::move(sock));
hb_->register_connection(conn->id(), now, [conn](TimeoutReason r){
    (void)r; conn->close();   // 踢线/回收：关闭 socket，触发 on_closed
});
conn->set_on_closed([this, id]{ hb_->unregister(id); /* + 从 active_ 移除 */ });
conn->start();
```

> 注：周一 `Connection` 已自带 per-socket 空闲定时器（默认 30s，作为 last-resort 守卫）。当 `HeartbeatManager` 作为权威活性治理上线时，应将 `Connection` 的 `idle_timeout_ms` 设为更大值以避免双重判定；二者当前并存属防御纵深，文档明示即可。

## 10. 开放问题

1. **端到端联调**：当前验证为管理器逻辑级（注入时钟）。真实"客户端心跳帧 → codec 调 `mark_activity` → 超时踢线"的链路需在 `ConnectionManager` 接入后做集成测。
2. **热路径去锁**：`mark_activity` 的互斥锁 → 下沉 `last_activity` 到 `Connection` 的 atomic，`tick` 只读（§6）。
3. **大 N 扫描优化**：5 万+ 连接时 `tick` 全表扫描可改为分片或时间轮（timer wheel），把 O(N) 降到 O(过期数)。
4. **服务端 PING**：当前为"被动检测沉默"；主动 PING/探活可在 `tick` 内对久未活动的连接下发探测帧（需 `Connection` 发字节能力），列为后续增强。
5. **idle_recycle 语义**：二次阈值默认关闭；生产若需"已认证但长期无业务的空闲回收"，设 `idle_recycle_timeout ≥ heartbeat_timeout` 即可。
