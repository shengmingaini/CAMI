#pragma once

#include "gateway/connection/connection_fsm.h"
#include "gateway/codec/frame_decoder.h"  // 帧解码器（每连接一个，线程亲和零锁）

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace cami {
namespace gateway {
namespace connection {

// 单条客户端连接 [PROTOTYPE→优化]
// 拥有 socket（RAII），生命周期绑定所属 io_context 线程（亲和性，不跨线程）。
// 职责边界（架构 §4.1）：仅"连接维持 + 生命周期状态机 + 空闲超时探测 + 帧定界"；
// 帧内的业务编解码/加解密归 codec/security 模块（decoder 为 codec 类，连接仅持有并调用）。
//
// [2026-08-12 性能优化]
//   - 内嵌 FrameDecoder：每连接一个解码器，on_frame 帧级回调（零拷贝视图），
//     消除集成层"全局 decoders map + 全局锁"（见 gateway-performance-review.md）。
//   - 定时器复用：async_wait 仅注册一次，mark_activity 只 expires_after 刷新
//     （被刷新取消的 aborted 回调内部自动重新挂起）——每消息省 1 次定时器操作。
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using Socket = boost::asio::ip::tcp::socket;
    // 帧级回调：payload 为解码器内部缓冲视图（生命周期限于回调内），
    // 需要异步持有/跨线程处理的消息必须由调用方显式拷贝。
    using FrameCallback = codec::FrameDecoder::FrameCallback;

    // 空闲超时阈值：超过该时长无任何活动（由数据到达刷新）即判失活断开。
    static constexpr std::uint32_t kDefaultIdleTimeoutMs = 30'000;

    explicit Connection(Socket socket,
                         std::uint32_t idle_timeout_ms = kDefaultIdleTimeoutMs);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 启动生命周期：Idle → Connecting → Handshaking(原型直转) → Established，并启动空闲超时定时器。
    void start();

    // 主动关闭：非终态 → Closing → Closed（幂等，已 Closed 直接返回）。
    void close();

    // 刷新空闲计时（防误杀长连接）。数据到达时由内部读循环自动调用；
    // 业务侧（如解析出心跳消息）也可显式调用。
    void mark_activity();

    // [集成] 帧级回调（推荐）：收到完整帧时调用（payload 视图零拷贝）。
    // 设置后，原始字节自动经内嵌 FrameDecoder 定界，不再透传 on_data_。
    void set_on_frame(FrameCallback cb) { on_frame_ = std::move(cb); }

    // [兼容] 原始字节回调：仅当未设置 on_frame 时生效（原型/自检路径）。
    void set_on_data(std::function<void(const std::uint8_t*, std::size_t)> cb) {
        on_data_ = std::move(cb);
    }

    // 在连接所属 io_context 线程上安全地执行 close()（跨线程调用时避免竞态，见 HeartbeatManager 踢线）。
    void close_via_executor();

    ConnectionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    std::uint64_t id() const noexcept { return id_; }

    // 对端地址（IP:port 的 IP 部分），供集成层按源 IP 限流/审计。
    // 只读访问器，不触任何连接逻辑（Week3 集成缝唯一必要的胶水点）。
    std::string peer_address() const;

    // 状态变更回调（供连接管理器统计/路由模块挂钩），可空。
    void set_state_change_callback(
        std::function<void(ConnectionState, ConnectionState)> cb) {
        on_state_change_ = std::move(cb);
    }
    // 终态回调（到达 Closed 时触发），供管理器从活动表移除本连接（避免悬空）。
    void set_on_closed(std::function<void()> cb) { on_closed_ = std::move(cb); }

private:
    void transition_to(ConnectionState to);
    void arm_idle_timer();      // 注册空闲超时等待（仅 start 时与 aborted 重挂时调用）
    void on_idle_timeout(const boost::system::error_code& ec);
    // 启动异步读取循环：async_read_some → 内嵌 decoder 分帧 → on_frame / on_data → 重新投递。
    void begin_read();

    Socket socket_;
    boost::asio::steady_timer idle_timer_;
    codec::FrameDecoder decoder_;              // 每连接一个解码器（线程亲和，零锁）
    std::array<std::uint8_t, 4096> read_buf_{};  // 固定读缓冲，避免运行时分配
    std::atomic<ConnectionState> state_{ConnectionState::kIdle};
    std::uint32_t idle_timeout_ms_;
    std::uint64_t id_;
    static std::atomic<std::uint64_t> next_id_;
    std::function<void(ConnectionState, ConnectionState)> on_state_change_;
    std::function<void()> on_closed_;
    std::function<void(const std::uint8_t*, std::size_t)> on_data_;  // 原始字节（兼容）
    FrameCallback on_frame_;                                         // 帧级回调（推荐）
};

}  // namespace connection
}  // namespace gateway
}  // namespace cami
