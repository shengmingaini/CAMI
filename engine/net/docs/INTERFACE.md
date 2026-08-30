# engine/net · INTERFACE

> **公开接口冻结契约（TASK-008 §7 / §27）**：下游只能 include 本文件声明的类型与
> 工厂函数；TcpTransport 等实现细节位于 `src/`，禁止直接引用（验收脚本静态扫描）。

## 常量（`transport.h`）

```cpp
inline constexpr uint32_t kLengthPrefixBytes = 4;   // 大端长度前缀（帧格式 §8）
inline constexpr uint32_t kMaxPayloadBytes  = 1 << 20;  // 1 MiB，超限拒绝
```

## 类型

### ConnectionId

```cpp
using ConnectionId = uint64_t;
```

- 编码 `(generation << 32) | slot_index`：同 slot 复用后 generation 递增，旧 id 必然失效。
- 全局单调：新分配的 id 严格大于此前所有 id。

### CloseReason

```cpp
enum class CloseReason : uint8_t {
    Graceful,    // 主动优雅关闭：发送缓冲排空后 FIN
    PeerClosed,  // 对端 FIN
    Error,       // 传输错误（RST 等）
    Timeout,     // 空闲超时被清理
    Rejected,    // 超出 max_connections 被拒绝
};
```

### TcpConfig（默认值 = §7 冻结值）

```cpp
struct TcpConfig {
    uint32_t io_threads{2};           // 第一版按 1 处理，字段保留
    uint32_t max_connections{50000};  // 超限拒绝并计数
    size_t   recv_buf{64 * 1024};     // 每连接接收缓冲（**必须 ≥ 期望最大帧**，否则超大帧被断开）
    size_t   send_buf{256 * 1024};    // 每连接发送缓冲（背压上限）
    bool     tcp_nodelay{true};
    uint32_t keepalive_idle_s{30};    // 空闲超时秒数（0 = 关闭）
    int32_t  backlog{1024};           // listen 队列
};
```

> **契约**：`recv_buf` 必须 ≥ 期望接收的最大帧（`kLengthPrefixBytes + payload`）。
> 若对端发送超过 `recv_buf` 容量的帧，连接按协议违规断开（避免永久半包卡死）。

### IConnection（发送/关闭入口）

```cpp
class IConnection {
    virtual ConnectionId Id() const noexcept = 0;
    virtual Result<void> Send(std::span<const uint8_t> data) = 0;
    virtual Result<void> Close(CloseReason reason) noexcept = 0;
    virtual std::string_view RemoteAddr() const noexcept = 0;
    virtual ConnectionStats Stats() const noexcept = 0;
};
```

- **Send**：传输层自动加 4B 长度前缀（与 Recv 剥前缀对称）。零拷贝语义 =
  写入发送缓冲，非阻塞。
  - 发送缓冲满 → `ErrorCode::BUSY`（上层限速/断开，禁止无限堆积）；
  - payload > `kMaxPayloadBytes` → `ErrorCode::INVALID_ARGUMENT`（上层须分片）；
  - 连接已关闭 → `ErrorCode::INVALID_ARGUMENT`。
- **Close**：`Graceful` 排空发送缓冲后 FIN；其余原因立即关闭。任意线程可调用（原子状态标记），幂等。
- **线程安全**：Send / Close 内部加锁，业务线程可安全调用；其余方法仅限 Poll 线程。

### TransportEvent（Poll 批量产出，禁止回调重入）

```cpp
struct TransportEvent {
    enum class Kind : uint8_t { Connected, Disconnected, Received, SendDrained, Error };
    Kind kind; ConnectionId conn_id; core::Error error; std::span<const uint8_t> data;
};
```

| Kind | data | error | 说明 |
|---|---|---|---|
| Connected | 空 | - | 新连接建立 |
| Received | payload（**仅下次 Poll 前有效**） | - | 已剥长度前缀 |
| SendDrained | 空 | - | 发送缓冲排空，上层可继续推流 |
| Disconnected | 空 | 关闭原因说明 | 之后该 conn_id 的 IConnection* 失效 |
| Error | 空 | 错误详情 | 连接级错误 |

> **生命周期红线**：`Received.data` 指向传输层暂存池，**下一次 Poll 调用即失效**；
> 必须在本轮 Poll 返回后立即消费或拷贝。

### INetworkTransport（上层唯一依赖）

```cpp
class INetworkTransport {
    virtual Result<void> Listen(std::string_view addr, uint16_t port) = 0;
    virtual Result<void> Stop() noexcept = 0;
    virtual Result<void> Poll(DurationMs timeout, std::vector<TransportEvent>& out) = 0;
    virtual IConnection* Get(ConnectionId id) noexcept = 0;
    virtual size_t ConnectionCount() const noexcept = 0;
    virtual TransportStats Stats() const noexcept = 0;
};
```

- **Poll**：由宿主线程（未来 Gateway 的 Network 角色）独占驱动。单次可产出多个事件
  （含同一连接的多个粘包 Received）。timeout 为最大阻塞时长。
- **Get**：**仅限驱动 Poll 的线程内调用**（连接表由 IO 线程独占）。返回的
  `IConnection*` 在对应 `Disconnected` 事件前有效，之后立即失效——上层须在
  Disconnected 时停止使用（存 id，不长期持有裸指针）。
- **Listen**：端口被占用返回 `INVALID_ARGUMENT`（Windows 用 `SO_EXCLUSIVEADDRUSE`，
  禁止重复绑定）；重复 Listen 返回 `INVALID_ARGUMENT`。

### 工厂

```cpp
std::unique_ptr<INetworkTransport> CreateTcpTransport(TcpConfig cfg);
```

- `max_connections == 0` → 返回 `nullptr`（非法配置）。
- 这是唯一允许出现 `TcpTransport` 字样的公开出口。

## 事件流示例

```
客户端 connect
  -> Poll: Connected(id=A)
  -> 宿主: conns[A] = Get(A)
客户端 send 帧
  -> Poll: Received(A, payload)   [可能同轮多个]
  -> 宿主: 处理消息
宿主 Send(A, resp)
  -> Poll: SendDrained(A)
宿主 Close(A, Graceful) / 对端 FIN / 超时 / 错误
  -> Poll: Disconnected(A, reason)
  -> 宿主: conns.erase(A)         // IConnection* 失效
```
