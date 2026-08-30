#pragma once

/// TASK-008 §15.3-8 —— TCP 传输实现（第一版，Windows WSAPoll / Linux poll 双平台）。
///
/// 线程模型（§4 / §9 / §7 注释）：
///   - Poll 由宿主线程（未来 Gateway 的 Network 角色线程）驱动；
///   - 连接表、poller、socket 状态全部由「调用 Poll 的线程」独占读写；
///   - 业务线程只能调用 IConnection::Send / Close（内部加锁），禁止触碰 socket；
///   - TcpConfig.io_threads 字段保留，第一版按 1 处理（后续版本再分区）。
///
/// 事件数据生命周期：Received 事件的 data 指向 event_pool_（本轮 Poll 内的暂存区），
/// **仅在下一次 Poll 调用前有效**（§7 契约）；调用方需在下一次 Poll 前消费。

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"
#include "mmo/net/connection.h"
#include "mmo/net/transport.h"
#include "poller.h"

namespace mmo::net {

/// 单连接在传输层内部的宿主对象：连接 + 事件暂存 + 活跃时间。
struct ConnHost {
    std::unique_ptr<TcpConnection> conn;
    core::SteadyNs last_active_ns{0};  // 最后一次收发活跃（单调时钟）
    bool want_write{false};            // 当前是否已注册可写关注
    bool in_use{false};
};

/// TCP 第一版实现。公开面只暴露 INetworkTransport（§21：上层只依赖抽象）。
class TcpTransport final : public INetworkTransport {
public:
    explicit TcpTransport(TcpConfig cfg);
    ~TcpTransport() override;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    core::Result<void> Listen(std::string_view addr, std::uint16_t port) override;
    core::Result<void> Stop() noexcept override;
    core::Result<void> Poll(core::DurationMs timeout,
                            std::vector<TransportEvent>& out) override;
    IConnection* Get(ConnectionId id) noexcept override;
    std::size_t ConnectionCount() const noexcept override;
    TransportStats Stats() const noexcept override;

private:
    core::Result<void> SetupListenSocket(std::string_view addr, std::uint16_t port);
    void ProcessAccept(std::vector<TransportEvent>& out);
    void ProcessRecv(ConnHost& host, std::vector<TransportEvent>& out);
    void ProcessSend(ConnHost& host, std::vector<TransportEvent>& out);
    void TryEmitPackets(ConnHost& host, std::vector<TransportEvent>& out);
    void CheckIdleTimeouts(std::vector<TransportEvent>& out);
    void RemoveConnection(ConnHost& host, CloseReason reason,
                          std::vector<TransportEvent>& out);
    void CloseAll(std::vector<TransportEvent>& out);

    /// 解析长度前缀（大端 4 字节）。返回 true 且 *len 有效；不足 4 字节返回 false。
    static bool PeekLength(const Buffer& buf, std::uint32_t& len);
    /// 从缓冲拷贝 n 字节到 dst（不消费，处理回绕）。
    static void CopyOut(const Buffer& buf, std::uint8_t* dst, std::size_t n);

    TcpConfig cfg_;
    SocketHandle listen_fd_{kInvalidSocket};
    bool listening_{false};

    Poller poller_;
    std::vector<ConnHost> slots_;            // SlotMap：slot 索引 -> ConnHost
    ConnectionIdAllocator id_alloc_;
    std::unordered_map<ConnectionId, std::size_t> slot_of_;  // id -> slot 索引（加速删除）
    std::unordered_map<SocketHandle, std::size_t> fd_to_slot_;  // fd -> slot 索引（poller 事件反查）
    std::deque<std::vector<std::uint8_t>> event_pool_;  // Received 事件数据池（本轮 Poll 有效）

    std::atomic<std::size_t> conn_count_{0};
    std::atomic<std::uint64_t> bytes_in_{0};
    std::atomic<std::uint64_t> packets_in_{0};
    std::atomic<std::uint64_t> error_count_{0};

    std::uint64_t frame_counter_{0};  // 测试/诊断用
};

}  // namespace mmo::net
