# engine/net · README

> **TASK-008 · Network Transport（TCP 第一版）**
> 面向 50k 并发目标的 MMORPG 传输层：事件驱动 + 抽象接口，上层只依赖
> `INetworkTransport` / `IConnection`，TCP 实现细节完全封装在模块内部。

## 是什么

`engine/net` 提供：

- **帧协议**（§8）：4 字节大端长度前缀 + payload（上限 `kMaxPayloadBytes = 1 MiB`）；
  发送自动加前缀、接收自动剥前缀——上层零编解码负担。
- **事件驱动的传输抽象**：`INetworkTransport::Poll` 批量产出
  `Connected / Disconnected / Received / SendDrained / Error` 事件，禁止回调重入。
- **TCP 第一版实现**：Windows `WSAPoll` / Linux `poll`，未来可替换为
  IOCP / epoll / QUIC / UDP 而不改动上层。
- **连接管理**：SlotMap + generation 的 `ConnectionId`（防 ABA），
  `max_connections` 超限拒绝并计数，空闲超时清理。
- **背压**：每连接发送缓冲上限（默认 256KB），满返回 `BUSY`，禁止无界增长。
- **大包安全**：帧长超过缓冲容量的连接被断开，不产生永久半包卡死。

## 快速上手

```cpp
#include "mmo/net/transport.h"   // 唯一需要的公开头

using namespace mmo::net;

TcpConfig cfg;                    // 默认 50k 上限 / 64KB recv / 256KB send
auto transport = CreateTcpTransport(cfg);

transport->Listen("127.0.0.1", 8080);   // 端口占用返回明确 Error，不崩溃

// 宿主线程驱动（Network 角色）：
std::vector<TransportEvent> events;
transport->Poll(std::chrono::milliseconds(10), events);
for (auto& ev : events) {
    switch (ev.kind) {
        case TransportEvent::Kind::Connected:
            conns_[ev.conn_id] = transport->Get(ev.conn_id);  // 保存 IConnection*
            break;
        case TransportEvent::Kind::Received:
            HandleMsg(ev.conn_id, ev.data);  // data 仅本次 Poll 前有效
            break;
        case TransportEvent::Kind::Disconnected:
            conns_.erase(ev.conn_id);        // 指针失效，停止使用
            break;
        default: break;
    }
}

// 业务线程：只调 Send / Close（内部加锁），禁止触碰 socket
conns_[id]->Send(payload);                    // 自动加 4B 长度前缀
conns_[id]->Close(CloseReason::Graceful);     // 排空发送缓冲后 FIN
```

## 关键设计

| 主题 | 决策 | 理由 |
|---|---|---|
| 线程模型 | Poll 由宿主线程独占；业务线程仅 Send/Close | §4 State Owner，连接表无锁 |
| 事件数据 | `Received.data` 指向本轮 Poll 暂存池，下轮 Poll 前有效 | 零拷贝、批量事件 |
| 缓冲 | 环形缓冲 + **延迟分配**（空闲连接 0 字节） | 10K 空闲连接 per-conn ≤ 20KB |
| 连接 ID | `(generation<<32) \| slot`，全局单调 | 防 ABA，杜绝误发旧连接 |
| 发送 | 写入发送缓冲（非阻塞），满 → `BUSY` | 背压，禁止无界增长 |
| 超大包 | 长度前缀 > 1MiB → 断开并计数 | 防协议滥用 / 内存耗尽 |

## 文档索引

- [INTERFACE.md](INTERFACE.md) —— 公开接口冻结契约与线程/生命周期规则
- [DEPENDENCY.md](DEPENDENCY.md) —— 模块依赖与红线
- [PERFORMANCE.md](PERFORMANCE.md) —— 1K/5K/10K 基准实测
- [TEST.md](TEST.md) —— 测试套件与覆盖清单
