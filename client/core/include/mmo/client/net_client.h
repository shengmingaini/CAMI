#pragma once

/// TASK-034 · 网络客户端（连接 / 重连 / 心跳 / 超时状态机）。
///
/// 设计要点：
///   - 单读线程模型：唯一一个 recv 线程独占 NetLink::Recv，负责解码 + 校验 +
///     把匹配 request_id 的 Response 投送到等待方（条件变量），其余信封按
///     Event 入事件队列。Send / Request 只在主线程写，避免双读竞争。
///   - Request() 阻塞等待匹配 Response（带超时）；超时或发送失败且开启重连时，
///     自动 Reconnect() 后重试一次；仍失败则转入 Disconnected。
///   - SendHeartbeat() 由调用方按心跳间隔驱动（游戏循环里定时调用），不在内部
///     另起定时器线程，便于确定性单测。
///   - 复用 TASK-005 的 mmo::protocol 编解码与 EnvelopeValidator；帧格式沿用
///     4 字节大端长度前缀（与 mmo::net / bot 对称）。
///   - 不依赖任何服务端模块。

#include "mmo/client/types.h"
#include "mmo/client/net_link.h"
#include "mmo/core/error/result.h"
#include "mmo/protocol/codec/envelope_view.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mmo { namespace protocol {
class ICodec;
}}  // namespace mmo::protocol

namespace mmo { namespace client {

/// 连接状态机。
enum class NetState : std::uint8_t {
    Disconnected = 0,
    Connecting,
    Connected,
    Reconnecting,
};

/// 网络客户端配置。
struct NetConfig {
    std::string     addr{"127.0.0.1:9001"};
    int             codec = 0;                  // 0=FlatBuffers（默认），1=Protobuf
    std::uint32_t   connect_timeout_ms = 2000;
    std::uint32_t   recv_timeout_ms = 1000;     // recv 轮询超时（用于心跳/事件探活）
    std::uint32_t   request_timeout_ms = 5000;  // Request 等待 Response 的上限
    std::uint32_t   heartbeat_interval_ms = 5000;
    bool            reconnect_enabled = true;
    int             max_reconnect_attempts = 5;
    std::uint32_t   reconnect_base_delay_ms = 500;
};

/// 网络指标（§19 Failure / §18 验收）。
struct NetMetrics {
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint64_t errors = 0;
    std::uint64_t reconnects = 0;
    double        rtt_ms_p95 = 0.0;
};

class NetClient {
public:
    explicit NetClient(NetConfig cfg = {});
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    NetState state() const noexcept { return state_.load(); }
    const NetMetrics& metrics() const noexcept { return metrics_; }

    /// 建立连接（含重连尝试）。成功转入 Connected 并启动 recv 线程。
    core::Result<void> Connect();
    /// 关闭（幂等）。
    void Disconnect() noexcept;

    /// 请求-响应式：编码并发送 Command 信封，阻塞等待匹配 Response。
    /// 失败且重连开启时自动重连后重试一次；仍失败转入 Disconnected。
    core::Result<std::vector<std::uint8_t>> Request(
        std::string_view payload, std::uint32_t timeout_ms = 0);

    /// 仅发送（Fire-and-forget）。
    core::Result<void> Send(std::string_view payload,
                            protocol::EnvelopeMessageType t =
                                protocol::EnvelopeMessageType::Command);

    /// 心跳：发送 Heartbeat 信封并等待 Heartbeat Response。
    core::Result<void> SendHeartbeat();

    /// 主动重连（关闭 + 带退避重连）。
    core::Result<void> Reconnect();

    /// 抽取服务器推送事件（Event 信封 payload）。非阻塞：立即返回队列现状。
    bool TryRecvEvent(std::vector<std::uint8_t>& out);

    /// 构造一个信封视图（自增 message_id / request_id）。
    protocol::EnvelopeView MakeEnvelope(protocol::EnvelopeMessageType t,
                                        std::uint64_t request_id,
                                        std::string_view payload);

private:
    // 单次连接（不含线程），供 Connect / Reconnect 复用。
    core::Result<void> ConnectOnce();
    void StopRecvThread() noexcept;
    void RecvLoop();
    // 收到一帧 payload 后的处理（在 recv 线程内）。
    void OnFrame(const std::vector<std::uint8_t>& frame);
    core::Result<std::vector<std::uint8_t>> WaitResponse(std::uint64_t request_id,
                                                         double sent_at_ms,
                                                         std::uint32_t timeout_ms);

    NetConfig cfg_;
    std::unique_ptr<protocol::ICodec> codec_;
    net::NetLink link_;

    mutable std::mutex mtx_;
    std::condition_variable resp_cv_;
    std::map<std::uint64_t, std::vector<std::uint8_t>> pending_resp_; // request_id -> payload
    std::deque<std::vector<std::uint8_t>> event_queue_;              // Event payloads

    std::uint64_t next_msg_id_{1};
    std::vector<double> rtt_samples_;

    std::atomic<NetState> state_{NetState::Disconnected};
    NetMetrics metrics_{};

    std::thread recv_thread_;
    std::atomic<bool> recv_running_{false};
};

}}  // namespace mmo::client
