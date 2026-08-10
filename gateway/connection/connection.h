#pragma once

#include "gateway/connection/connection_fsm.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace cami {
namespace gateway {
namespace connection {

// 单条客户端连接 [PROTOTYPE]
// 拥有 socket（RAII），生命周期绑定所属 io_context 线程（亲和性，不跨线程）。
// 职责边界（架构 §4.1）：仅"连接维持 + 生命周期状态机 + 空闲超时探测"；
// 消息编解码/加解密归 codec/security 模块，本模块不碰字节流。
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using Socket = boost::asio::ip::tcp::socket;

    // 空闲超时阈值：超过该时长无任何活动（由 codec 调 mark_activity 刷新）即判失活断开。
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

    // codec 模块每成功解码一条消息调用，刷新空闲计时（防误杀长连接）。
    void mark_activity();

    ConnectionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    std::uint64_t id() const noexcept { return id_; }

    // 状态变更回调（供连接管理器统计/路由模块挂钩），可空。
    void set_state_change_callback(
        std::function<void(ConnectionState, ConnectionState)> cb) {
        on_state_change_ = std::move(cb);
    }
    // 终态回调（到达 Closed 时触发），供管理器从活动表移除本连接（避免悬空）。
    void set_on_closed(std::function<void()> cb) { on_closed_ = std::move(cb); }

private:
    void transition_to(ConnectionState to);
    void start_idle_timer();
    void on_idle_timeout(const boost::system::error_code& ec);

    Socket socket_;
    boost::asio::steady_timer idle_timer_;
    std::atomic<ConnectionState> state_{ConnectionState::kIdle};
    std::uint32_t idle_timeout_ms_;
    std::uint64_t id_;
    static std::atomic<std::uint64_t> next_id_;
    std::function<void(ConnectionState, ConnectionState)> on_state_change_;
    std::function<void()> on_closed_;
};

}  // namespace connection
}  // namespace gateway
}  // namespace cami
