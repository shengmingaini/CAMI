#include "gateway/connection/connection.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>

namespace cami {
namespace gateway {
namespace connection {

std::atomic<std::uint64_t> Connection::next_id_{1};

Connection::Connection(Socket socket, std::uint32_t idle_timeout_ms)
    : socket_(std::move(socket)),
      idle_timer_(socket_.get_executor()),
      idle_timeout_ms_(idle_timeout_ms),
      id_(next_id_.fetch_add(1, std::memory_order_relaxed)) {}

Connection::~Connection() {
    // 析构直接落终态，不经 transition_to（避免析构期再触发 on_closed_ 回调造成重入）。
    state_.store(ConnectionState::kClosed, std::memory_order_release);
}

void Connection::start() {
    transition_to(ConnectionState::kConnecting);
    transition_to(ConnectionState::kHandshaking);
    // [PROTOTYPE] 握手 hook：真实环境由 codec/security 模块填充（架构 §4.1）。
    // 原型阶段直接置 Established，仅验证生命周期链路。
    transition_to(ConnectionState::kEstablished);
    start_idle_timer();
    begin_read();  // [集成] 进入 Established 即开始异步读取，原始字节经 on_data 上抛。
}

void Connection::mark_activity() {
    if (state_.load(std::memory_order_acquire) == ConnectionState::kEstablished) {
        start_idle_timer();
    }
}

void Connection::close() {
    // 保活：防止 on_closed_ 删除 this 后继续访问成员（见 transition_to）。
    auto self = shared_from_this();
    (void)self;
    ConnectionState cur = state_.load(std::memory_order_acquire);
    if (cur == ConnectionState::kClosed) return;
    if (can_transition(cur, ConnectionState::kClosing)) {
        transition_to(ConnectionState::kClosing);
    }
    transition_to(ConnectionState::kClosed);
    boost::system::error_code ec;
    socket_.close(ec);  // 尽力而为，忽略错误
    idle_timer_.cancel();
}

void Connection::transition_to(ConnectionState to) {
    ConnectionState from = state_.load(std::memory_order_acquire);
    if (!can_transition(from, to)) {
        // 非法迁移：强制 Error 终态，禁止静默状态错乱。
        state_.store(ConnectionState::kError, std::memory_order_release);
        if (on_state_change_) on_state_change_(from, ConnectionState::kError);
        return;
    }
    state_.store(to, std::memory_order_release);
    if (on_state_change_) on_state_change_(from, to);
    if (to == ConnectionState::kClosed && on_closed_) on_closed_();
}

void Connection::start_idle_timer() {
    // 注意：现代 Boost.Asio 的 expires_after 仅接受 duration 单参数（无 error_code 重载）。
    idle_timer_.expires_after(std::chrono::milliseconds(idle_timeout_ms_));
    auto self = shared_from_this();
    idle_timer_.async_wait(
        [self](const boost::system::error_code& ec) { self->on_idle_timeout(ec); });
}

void Connection::on_idle_timeout(const boost::system::error_code& ec) {
    if (ec == boost::asio::error::operation_aborted) return;  // 被 mark_activity/cancel 取消，忽略
    // 空闲超时 → 失活 → 关闭
    transition_to(ConnectionState::kClosing);
    transition_to(ConnectionState::kClosed);
    boost::system::error_code close_ec;
    socket_.close(close_ec);
}

void Connection::begin_read() {
    if (state_.load(std::memory_order_acquire) != ConnectionState::kEstablished) return;
    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(read_buf_),
        [self](const boost::system::error_code& ec, std::size_t n) {
            if (ec) {
                // operation_aborted（close 取消）/ EOF / 其他错误：停止读取，不重入循环。
                return;
            }
            if (self->on_data_) {
                self->on_data_(self->read_buf_.data(), n);
            }
            // 继续投递下一次读取（单连接单线程亲和，无并发读竞态）。
            self->begin_read();
        });
}

void Connection::close_via_executor() {
    // 在连接所属 io_context 线程上执行 close()，避免跨线程操作 socket/timer 的竞态
    //（例如 HeartbeatManager 在 pool timer 线程判定超时后踢线）。
    auto ex = socket_.get_executor();
    boost::asio::dispatch(ex, [self = shared_from_this()]() { self->close(); });
}

}  // namespace connection
}  // namespace gateway
}  // namespace cami
