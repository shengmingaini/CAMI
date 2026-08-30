# engine/net · DEPENDENCY

> TASK-008 §27 模块边界：依赖方向单向无环；下游禁止 include 本模块 `src/`；
> 本模块禁止访问依赖模块内部数据。

## 依赖图

```
engine/net (mmo::net)
   ├── engine/core · mmo::core_error   (Result/Error/ErrorCode)
   ├── engine/core · mmo::core_time    (MonotonicClock / DurationMs / SteadyNs)
   └── Windows 平台库：ws2_32（PRIVATE）
```

- **mmo::net 不依赖 engine/rpc、engine/protocol**（独立于 gRPC 链路）。
- TASK-008 消费上游：TASK-004 `engine/core`（§27.2）；协议层（TASK-005）不参与
  传输帧格式——帧格式（4B 长度前缀）由传输层负责（§8 Packet）。

## 边界红线（统一，§27.3）

| 红线 | 检查方式 |
|---|---|
| 公开头不 include `src/` | 验收脚本 `scan_forbidden engine/net/include 'src/'` |
| 上层只依赖 `INetworkTransport` 抽象 | grep：除 factory 外无 `TcpTransport` 直接引用 |
| 不扩散到他人 module 子树 | 本模块只写 `engine/net/` |
| 传输层无游戏语义 | grep：net/ 无 combat/scene/quest 字样 |
| IO 线程不做业务解包 | Poll 只产事件，解码限帧格式 |
| 发送缓冲无界增长非法 | 满返回 BUSY，Buffer 不扩容 |

## 平台差异（第一版）

| 项 | Windows | Linux |
|---|---|---|
| poll 实现 | WSAPoll | poll(2) |
| 端口独占 | `SO_EXCLUSIVEADDRUSE`（禁止重复绑定） | `SO_REUSEADDR`（TIME_WAIT 容忍） |
| 端口占用错误 | `WSAEADDRINUSE` / `WSAEACCES` → INVALID_ARGUMENT | `EADDRINUSE` → INVALID_ARGUMENT |
| 非阻塞 | `ioctlsocket(FIONBIO)` | `fcntl(O_NONBLOCK)` |
| socket 关闭 | `closesocket` | `close` |
| 启动 | 一次性 WSAStartup（进程级幂等） | — |

> 注意：Windows 上 `SO_REUSEADDR` 允许第二个 socket 重复绑定同一端口（与 POSIX
> 语义相反），因此第一版强制 `SO_EXCLUSIVEADDRUSE`，保证「端口占用必须报错」契约。

## 线程所有权（§4 State Owner）

| 数据 | 权威写入者 | 其他访问者 |
|---|---|---|
| 连接表 / poller / socket 状态 | Poll 线程（独占） | 无 |
| 接收缓冲 | Poll 线程（无锁） | 无 |
| 发送缓冲 | Poll 线程（DrainSend） | 业务线程（Send，加锁） |
| 连接状态机（Open/Closing/Closed） | 任意线程（原子 CAS） | 只读 |
| 统计计数 | 各线程（原子累加） | 只读聚合 |

## 已知限制（第一版，如实记录）

1. `io_threads` 字段保留但按 1 处理：单 Poll 线程。多 IO 分区（sharding）留给后续版本。
2. 无 recv 侧流控：上层必须及时消费 Received 事件（缓冲满且帧超容量 → 断开）。
3. `WSAPoll` 就绪事件快照全量扫描（O(n)）；IOCP 留作后续优化。
4. 空闲连接采用延迟分配缓冲（0 字节），首次收发才分配；`Reset` 时释放。
