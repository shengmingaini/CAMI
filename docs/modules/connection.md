# 连接管理模块 详细设计

> **文档状态**: [PROTOTYPE]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-10 (Week2 周一 — 网关·连接管理)  
> **所属层**: 接入层 (Gateway)  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, 16 ADR)  
> **协议契约**: `docs/protocols/protocol-spec.md`（消息层，本模块不消费字节流）  
> **关联 ADR**: ADR-014（QUIC + 边缘网关，v3.0）

---

## 1. 模块概述

- **定位**：接入层（Gateway）的连接管理子模块，负责客户端连接的"接入维持 + 生命周期管理 + 空闲超时探测"，是后续编解码/安全/路由/迁移模块的地基。
- **核心职责**：
  1. **io_context 线程池**：N 个 `boost::asio::io_context`，每线程一个、无 work-stealing，配合 `SO_REUSEPORT` 由内核做连接负载均衡（无状态水平扩展）。
  2. **连接生命周期状态机**：显式状态迁移（Idle→Connecting→Handshaking→Established→Closing→Closed，异常→Error），非法迁移强制落 Error 终态，禁止静默错乱。
  3. **SO_REUSEPORT 监听池**：每个 io_context 一个 `acceptor`，全部 `bind` 同一 `endpoint` 并启用端口复用；内核把新建连接分发到不同线程，无全局锁。
- **不在职责内（边界）**：
  - **不读字节流**：消息编解码（FlatBuffers / 位压缩）、AES 加解密归 **codec / security** 模块（后续任务）。本模块 `Connection` 仅在 Established 后挂空闲定时器，通过 `mark_activity()` 接口由 codec 心跳刷新。
  - **不做路由/迁移**：连接建立后如何路由到 GameNode、GameNode 宕机如何迁移，归 **router / migration** 模块（后续任务）。本模块只把新连接交给 `on_state_change_` 回调，由上层挂钩。
  - **零玩家持久化**：符合架构红线（接入层不存玩家数据）。

## 2. 架构约束与边界

> 引用架构 §4.1 接入层职责 + ADR-014 无状态水平扩展。

- **是否拥有 `Player` 对象**：否（接入层红线，零玩家持久化）。
- **是否拥有 socket**：是（每个 `Connection` RAII 拥有其 `tcp::socket`），但 socket 仅做"维持 + 关闭"，不解析协议。
- **红线禁令**：禁止全局锁（accept 分发靠内核 SO_REUSEPORT，连接内状态靠单线程亲和 + 原子；活动表用 `std::mutex` 仅覆盖 accept/close 冷路径）；禁止同步阻塞 IO（全程 `async_accept` / `async_wait`）；禁止无 AOI 全服广播（本层无广播语义）。

## 3. 对外接口（C++ 签名级）

命名规范（架构 §8.1 / 编码规范 v1）：类 `PascalCase`，方法 `camelCase`，命名空间 `cami::gateway::connection`。

### 3.1 连接管理器（入口）
```cpp
class ConnectionManager {
public:
    explicit ConnectionManager(std::size_t pool_size, int listen_backlog = 1024);
    int start(const std::string& host, std::uint16_t port);  // 返回 0 成功，非 0=错误码
    void stop();
    std::size_t pool_size() const noexcept;
};
```

### 3.2 单连接（生命周期）
```cpp
class Connection : public std::enable_shared_from_this<Connection> {
public:
    void start();                       // 启动状态机 + 空闲定时器
    void close();                       // 主动关闭（幂等）
    void mark_activity();               // codec 每解码一条消息调用，刷新空闲计时
    ConnectionState state() const noexcept;
    std::uint64_t id() const noexcept;
    void set_state_change_callback(std::function<void(ConnectionState, ConnectionState)>);
    void set_on_closed(std::function<void()>);  // 终态时回调，供管理器从活动表移除
};
```

### 3.3 线程池
```cpp
class IoContextPool {
public:
    explicit IoContextPool(std::size_t pool_size = 1);  // 0 → 退化为 1
    void run();   // 启动线程（可重入安全）
    void stop();  // 停止并 join（可重入安全）
    boost::asio::io_context& get_io_context();  // RR 分配
    std::size_t size() const noexcept;
};
```

### 3.4 自检（CI 机器证据）
```cpp
bool connection_selfcheck();  // FSM 合法性 + 线程池启停，不绑真实端口
```

## 4. 核心数据结构

- **状态机**：`enum class ConnectionState { kIdle, kConnecting, kHandshaking, kEstablished, kClosing, kClosed, kError }`，迁移合法性由 `can_transition(from, to)` 编译期表达、运行期校验。
- **Connection**：`tcp::socket`（RAII）+ `steady_timer`（空闲超时）+ `std::atomic<ConnectionState>`（无锁读状态）+ 自增 `id_`（原子分配）。**活动表** `std::vector<std::shared_ptr<Connection>>` 由 `std::mutex` 保护（冷路径：仅 accept / close 触及）。
- **IoContextPool**：`unique_ptr<io_context>[]` + `executor_work_guard[]`（防 run() 空转退出）+ `thread[]` + 原子 `next_index_`（RR）。

## 5. 事件契约（进程内 EventBus）

本模块 [PROTOTYPE] 阶段**不直接发 EventBus 事件**（连接事件后续由 router/migration 模块经事件总线解耦，参考 ADR-001/012）。当前通过 C++ 回调 `on_state_change_` / `on_closed_` 与上层解耦，避免引入跨模块硬编码依赖。

| 本模块对外回调 | 触发时机 | 消费者 |
|----------------|----------|--------|
| `on_state_change_(from, to)` | 状态迁移 | 连接管理器统计 / 路由模块挂钩 |
| `on_closed_()` | 到达 Closed | 连接管理器从活动表移除 |

## 6. 协议引用

本模块不引用任何 `MessageBody` 枚举——不解析协议字节。消息格式由 `docs/protocols/protocol-spec.md` 定义，交给 codec 模块。高频逐帧移动走位压缩轨道 A（protocol-spec §7），与本模块无关。

## 7. 并发模型

- **线程归属**：每个 `Connection` 绑定其 `accept` 所在的 io_context 线程（亲和性），生命周期内**不跨线程迁移**，故 socket 访问无需锁；状态读用 `std::atomic` 即可。
- **负载均衡**：N 个 acceptor 各自 `bind` 同端口（`SO_REUSEPORT` / `reuse_address`），内核把新建连接分发到不同 acceptor 线程 —— **无全局锁、无同步阻塞**。
- **活动表锁**：`active_` 的增删发生在 accept / close（冷路径，远低于消息热路径），用 `std::mutex` 完全可接受（编码规范 v1 §5：冷路径可加锁）。
- **保活与析构安全**：`Connection` 经 `shared_from_this` 保活；`on_closed_` 捕获裸指针避免 `shared_ptr` 环；析构直接落 `kClosed` 不触发回调，防止重入。

## 8. 性能预算

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 单节点连接数 [v3.0] | 5000（中心网关） | acceptor 池 + 活动表，O(1) accept；容量由 `pool_size`/核数决定 |
| 连接迁移时间 | ≤ 800ms | 本模块仅负责"识别失效并关闭"，迁移逻辑归 migration 模块 |
| 单节点消息 [v3.0] | ≤ 20,000/s | 连接维持无热路径锁；实际消息处理在 codec/game 层 |
| 客户端重连 [v3.0] | ~0ms（QUIC 0-RTT） | **已知差距**：本 [PROTOTYPE] 为 TCP 基础，QUIC 0-RTT 见 §11 开放问题 |

## 9. 依赖方向

```
[上层调用方 / 路由模块] ──→ [ConnectionManager] ──→ [IoContextPool / Connection / FSM]
                                         │
                                         └──→ Boost.Asio (io_context / socket)
```
- **上游（调用本模块）**：gateway 主入口、后续 router 模块。
- **下游（本模块调用）**：Boost.Asio（仅 header + `boost::system`）、`common`（层链接锚点，间接）。禁止逆向回调。
- **单向依赖**：`gateway → common`，本模块不反向依赖 game/data。

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 接入层无状态水平扩展 / QUIC 边缘网关 | ADR-014 (v3.0) |
| 模块边界 WoW 模式（不拥有 Player） | ADR-002 |
| 进程内 EventBus 解耦（本模块暂用回调，事件后续归 router/migration） | ADR-001 / ADR-012 |

## 11. 开放问题 / 后续

- **[PROTOTYPE→PRODUCTION] QUIC 升级路径**：v3.0 要求 QUIC（UDP + 0-RTT + 连接迁移）。本模块当前为 **TCP 基础脚手架**，SO_REUSEPORT 对 UDP 同样适用；后续把 `Connection` 底层 socket 从 `tcp::socket` 替换为 QUIC 会话（或抽象 `Transport` 接口），握手状态（`kHandshaking`）接入真实加密协商，连接迁移接入 `migration` 模块。
- **Windows SO_REUSEPORT 缺失**：MinGW 无 `SO_REUSEPORT`，当前退化为 `SO_REUSEADDR`，多 acceptor 同端口的内核负载均衡在 Windows 上有限；生产部署以 Linux 为准（已在 `connection_manager.cpp` 标注）。
- **心跳/限流**：空闲超时探测已实现；令牌桶限流（单 IP 100req/s）归 security 模块。
- **[PROTOTYPE]/[PRODUCTION] 标注**：本模块全部类/函数标注 `[PROTOTYPE]`，接入真实 QUIC + codec 后升 [PRODUCTION]。
- **压测**：现有 `benchmark/tcp_echo_*` 可复用做 accept 吞吐基准；5 万并发验证脚本见 `scripts/benchmark/`（架构 §10.2）。
