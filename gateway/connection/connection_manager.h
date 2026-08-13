#pragma once

#include "gateway/connection/connection.h"
#include "gateway/connection/io_context_pool.h"

#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cami {
namespace gateway {
namespace connection {

// 连接管理器 [PROTOTYPE]
// 每个 io_context 一个 acceptor，全部绑定同一 endpoint 并启用 SO_REUSEPORT(Linux)
// / reuse_address(Windows)；由内核做连接负载均衡（架构 §4.1 / ADR-014 无状态水平扩展）。
// 无状态水平扩展：多进程/多线程各自 bind 同端口，内核分发，无全局锁。
class ConnectionManager {
public:
    // pool_size: io_context 线程数（建议 = CPU 核数）；listen_backlog: listen() 队列长度；
    // idle_timeout_ms: 新建连接的空闲超时（默认 30s，心跳场景传心跳宽限）。
    explicit ConnectionManager(std::size_t pool_size, int listen_backlog = 1024,
                               std::uint32_t idle_timeout_ms =
                                   Connection::kDefaultIdleTimeoutMs);

    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    // 在 host:port 启动监听（每个 io_context 一个 acceptor）。返回 0 成功，非 0 = 错误码。
    int start(const std::string& host, std::uint16_t port);

    // 停止接受新连接并关闭所有 acceptor；已建连接由各自空闲超时/外部 close 自然结束。
    void stop();

    // [集成挂钩] 每条连接 accepted 后、start() 之前调用，供上层设置 on_data / 注册心跳等。
    // 传入 shared_ptr<Connection> 便于上层在踢线闭包中安全持有并 close。
    void set_on_accept(std::function<void(std::shared_ptr<Connection>)> cb) {
        on_accept_ = std::move(cb);
    }
    // [集成挂钩] 连接到达终态(Closed)时先回调（上层做心跳注销/解码器清理），随后自动从 active_ 移除。
    void set_on_connection_closed(std::function<void(Connection&)> cb) {
        on_conn_closed_ = std::move(cb);
    }

    std::size_t pool_size() const noexcept { return pool_.size(); }

private:
    void do_accept(boost::asio::ip::tcp::acceptor& acceptor);

    IoContextPool pool_;
    int listen_backlog_;
    std::uint32_t idle_timeout_ms_;
    std::vector<std::unique_ptr<boost::asio::ip::tcp::acceptor>> acceptors_;
    std::mutex active_mtx_;
    std::vector<std::shared_ptr<Connection>> active_;  // 冷路径（accept/close），可加锁
    std::function<void(std::shared_ptr<Connection>)> on_accept_;  // 上层集成挂钩
    std::function<void(Connection&)> on_conn_closed_;              // 上层终态清理挂钩
};

}  // namespace connection
}  // namespace gateway
}  // namespace cami
