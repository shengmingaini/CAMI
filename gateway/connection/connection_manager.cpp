#include "gateway/connection/connection_manager.h"

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>

#if defined(__linux__)
#include <sys/socket.h>  // SO_REUSEPORT / SOL_SOCKET / socklen_t（仅 Linux 编译块需要）
#endif

#include <iostream>  // 仅启动/错误期日志；业务热路径禁止（编码规范 §4）

namespace cami {
namespace gateway {
namespace connection {

namespace {

// 跨平台端口复用：Linux 原生 SO_REUSEPORT；Windows(MinGW) 无该选项，
// 退化为 reuse_address(true)（= SO_REUSEADDR）。多 acceptor 同端口的负载均衡
// 在 Windows 上有限，已在连接模块文档标注为已知限制。
void set_reuseport(boost::asio::ip::tcp::acceptor& acceptor) {
    boost::system::error_code ec;
    acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
#if defined(__linux__)
    int one = 1;
    if (::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT,
                     &one, static_cast<socklen_t>(sizeof(one))) != 0) {
        // 非致命：部分容器/内核不支持，已退化为 reuse_address。
    }
#endif
}

}  // namespace

ConnectionManager::ConnectionManager(std::size_t pool_size, int listen_backlog)
    : pool_(pool_size), listen_backlog_(listen_backlog) {}

int ConnectionManager::start(const std::string& host, std::uint16_t port) {
    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address(host), port);
    pool_.run();  // 启动 io_context 线程（重复调用安全）
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        boost::asio::io_context& ctx = pool_.get_io_context();  // 轮询分配，每 context 一个 acceptor
        auto acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(ctx);
        set_reuseport(*acceptor);
        boost::system::error_code ec;
        acceptor->open(ep.protocol(), ec);
        if (!ec) acceptor->bind(ep, ec);
        if (!ec) acceptor->listen(listen_backlog_, ec);
        if (ec) {
            std::cerr << "[connection] acceptor bind/listen failed: " << ec.message()
                      << "\n";
            return ec.value();
        }
        acceptors_.push_back(std::move(acceptor));
        do_accept(*acceptors_.back());
    }
    return 0;
}

void ConnectionManager::stop() {
    // 先停 io_context 并 join 线程：pending accept 回调在 acceptors_ 仍存活期间以
    // operation_aborted 结束，避免悬空引用；之后再释放 acceptor。
    pool_.stop();
    for (auto& a : acceptors_) {
        boost::system::error_code ec;
        a->close(ec);
    }
    acceptors_.clear();
}

void ConnectionManager::do_accept(boost::asio::ip::tcp::acceptor& acceptor) {
    acceptor.async_accept(
        [&acceptor, this](boost::system::error_code ec,
                          boost::asio::ip::tcp::socket socket) {
            if (ec) {
                // operation_aborted = 管理器 stop() 关闭 acceptor；其余错误仅跳过本条，不中断池。
                return;
            }
            auto conn = std::make_shared<Connection>(std::move(socket));
            // 终态回调：从活动表移除。捕获裸指针避免 shared_ptr 环；manager 生命期覆盖连接。
            conn->set_on_closed([this, raw = conn.get()]() {
                std::lock_guard<std::mutex> lk(active_mtx_);
                for (auto it = active_.begin(); it != active_.end(); ++it) {
                    if (it->get() == raw) { active_.erase(it); break; }
                }
            });
            conn->start();
            {
                std::lock_guard<std::mutex> lk(active_mtx_);
                active_.push_back(conn);
            }
            do_accept(acceptor);  // 继续接受下一条
        });
}

}  // namespace connection
}  // namespace gateway
}  // namespace cami
