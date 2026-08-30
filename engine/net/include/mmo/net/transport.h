#pragma once

/// TASK-008 §7 Public Interface —— 网络传输抽象层。
///
/// 设计要点：
///   - 上层**只依赖本文件**（INetworkTransport / IConnection），禁止直接引用 TcpTransport；
///   - 事件驱动：TransportEvent 经 Poll 批量产出，**禁止回调重入业务代码**（§21）；
///   - Connection 状态由 IO 线程独占写入（§4 State Owner），业务线程只能 Send / Close 提交请求；
///   - Send 为「写入发送缓冲」的零拷贝语义（§7 注释），由传输实现决定线程安全边界；
///   - 本文件只做接口与纯数据结构声明，不包含任何平台相关代码。

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"

namespace mmo::net {

using ConnectionId = std::uint64_t;

/// 帧格式（§8 Data Model）：4 字节大端长度前缀 + payload。
/// Recv 侧剥前缀产出 payload；Send 侧自动加前缀（对称契约，上层零编解码负担）。
inline constexpr std::uint32_t kLengthPrefixBytes = 4;
/// 最大 payload 长度（§16 大包 1MB；长度前缀超限即拒绝并断开）。
inline constexpr std::uint32_t kMaxPayloadBytes = 1 << 20;  // 1 MiB

/// 连接关闭原因（供上层区分主动/被动/异常）。
enum class CloseReason : std::uint8_t {
    Graceful = 0,   // 主动优雅关闭（发送缓冲排空后 FIN）
    PeerClosed = 1, // 对端主动关闭（收到 FIN）
    Error = 2,      // 传输错误（RST 等）
    Timeout = 3,    // 空闲超时被清理
    Rejected = 4,   // 超出 max_connections 被拒绝
};

/// 单连接统计（§8 TransportStats 的连接维度）。
struct ConnectionStats {
    std::uint64_t bytes_in{0};
    std::uint64_t bytes_out{0};
    std::uint64_t packets_in{0};
    std::uint64_t packets_out{0};
    std::uint64_t send_queue_bytes{0};  // 当前发送缓冲占用
    std::uint64_t error_count{0};
};

/// 传输层整体统计（§8）。
struct TransportStats {
    std::uint64_t conn_count{0};
    std::uint64_t bytes_in{0};
    std::uint64_t bytes_out{0};
    std::uint64_t packets_in{0};
    std::uint64_t packets_out{0};
    std::uint64_t send_queue_depth{0};  // 全部连接发送队列总字节
    std::uint64_t error_count{0};
};

/// TcpConfig：TCP 第一版配置（§7 默认值）。
struct TcpConfig {
    std::uint32_t io_threads{2};          // IO 线程数（第一版实现按 1 处理，字段保留）
    std::uint32_t max_connections{50000}; // 连接数上限，超限拒绝并计数
    std::size_t recv_buf{64 * 1024};      // 每连接接收缓冲
    std::size_t send_buf{256 * 1024};     // 每连接发送缓冲（背压上限）
    bool tcp_nodelay{true};
    std::uint32_t keepalive_idle_s{30};   // 空闲超时（秒）
    std::int32_t backlog{1024};           // listen 队列
};

/// 传输事件：Poll 批量产出，宿主线程逐条处理。
/// data 的生命周期：仅对 Received 有效，指向传输层内部缓冲，
/// **仅在下一次 Poll 调用前有效**（事件消费后即失效）。
struct TransportEvent {
    enum class Kind : std::uint8_t {
        Connected,     // 新连接建立（data 为空）
        Disconnected,  // 连接关闭（data 为空，error 携带关闭原因说明）
        Received,      // 收到一段数据（data 有效）
        SendDrained,   // 发送缓冲排空（上层可据此继续推流）
        Error,         // 连接级错误（error 有效）
    };
    TransportEvent() = default;
    Kind kind{Kind::Error};
    ConnectionId conn_id{0};
    core::Error error{core::ErrorCode::OK, "", core::domain::kNet};
    std::span<const std::uint8_t> data;
};

class IConnection {
public:
    virtual ~IConnection() = default;

    virtual ConnectionId Id() const noexcept = 0;
    /// 发送一条消息（传输层自动加 4B 长度前缀，与 Recv 剥前缀对称）。
    /// 零拷贝语义：写入发送缓冲，非阻塞；缓冲满返回 BUSY（上层限速/断开），
    /// payload 超过 kMaxPayloadBytes 返回 INVALID_ARGUMENT（上层须自行分片）。
    virtual core::Result<void> Send(std::span<const std::uint8_t> data) = 0;
    /// 请求关闭。Graceful = 排空发送缓冲后 FIN；其余原因直接 RST/关闭。
    virtual core::Result<void> Close(CloseReason reason) noexcept = 0;
    virtual std::string_view RemoteAddr() const noexcept = 0;
    virtual ConnectionStats Stats() const noexcept = 0;
};

class INetworkTransport {
public:
    virtual ~INetworkTransport() = default;

    /// 绑定并监听。端口被占用返回明确 Error（不崩溃）。
    virtual core::Result<void> Listen(std::string_view addr, std::uint16_t port) = 0;
    /// 停止：关闭监听、优雅关闭所有连接、回收 IO 线程。
    virtual core::Result<void> Stop() noexcept = 0;
    /// 由宿主线程驱动：等待并收集一批事件（timeout 为最大阻塞时长）。
    virtual core::Result<void> Poll(core::DurationMs timeout,
                                    std::vector<TransportEvent>& out) = 0;
    /// 按 id 取连接对象（发送/关闭入口）。**仅可在驱动 Poll 的线程内调用**
    /// （连接表由 IO 线程独占，见 §4）；返回的指针在对应 Disconnected 事件
    /// 发出前有效，连接回收后立即失效——上层须在 Disconnected 时停止使用。
    /// id 无效或连接已移除返回 nullptr。
    virtual IConnection* Get(ConnectionId id) noexcept = 0;
    virtual std::size_t ConnectionCount() const noexcept = 0;
    virtual TransportStats Stats() const noexcept = 0;
};

/// 工厂：创建 TCP 第一版实现。失败返回明确 Error（配置非法等）。
std::unique_ptr<INetworkTransport> CreateTcpTransport(TcpConfig cfg);

}  // namespace mmo::net
