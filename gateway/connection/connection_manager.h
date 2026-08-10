#pragma once

#include "gateway/connection/connection.h"
#include "gateway/connection/io_context_pool.h"

#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
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
    // pool_size: io_context 线程数（建议 = CPU 核数）；listen_backlog: listen() 队列长度。
    explicit ConnectionManager(std::size_t pool_size, int listen_backlog = 1024);

    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    // 在 host:port 启动监听（每个 io_context 一个 acceptor）。返回 0 成功，非 0 = 错误码。
    int start(const std::string& host, std::uint16_t port);

    // 停止接受新连接并关闭所有 acceptor；已建连接由各自空闲超时/外部 close 自然结束。
    void stop();

    std::size_t pool_size() const noexcept { return pool_.size(); }

private:
    void do_accept(boost::asio::ip::tcp::acceptor& acceptor);

    IoContextPool pool_;
    int listen_backlog_;
    std::vector<std::unique_ptr<boost::asio::ip::tcp::acceptor>> acceptors_;
    std::mutex active_mtx_;
    std::vector<std::shared_ptr<Connection>> active_;  // 冷路径（accept/close），可加锁
};

}  // namespace connection
}  // namespace gateway
}  // namespace cami
