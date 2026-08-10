#pragma once

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace cami {
namespace gateway {
namespace connection {

// [PROTOTYPE] io_context 线程池 —— 一个 io_context 绑定一个 OS 线程，无 work-stealing。
// 配合 SO_REUSEPORT 由内核做连接/数据报负载均衡（架构 §4.1 / ADR-014 边缘网关无状态水平扩展）。
// 关联原生 socket 的目标须链 ws2_32/mswsock（Windows, WIN32 守卫）—— 见本目录 CMakeLists。
class IoContextPool {
public:
    // pool_size 通常取 CPU 核数；为 0 时退化为 1（单线程），不崩溃。
    explicit IoContextPool(std::size_t pool_size = 1);
    ~IoContextPool();

    IoContextPool(const IoContextPool&) = delete;
    IoContextPool& operator=(const IoContextPool&) = delete;

    // 启动所有 io_context 运行线程。可重入保护：重复调用无效。
    void run();
    // 请求停止（io_context::stop）并 join 所有线程。可重入安全。
    void stop();

    // 轮询取一个 io_context（RR），用于为新连接分配固定 io_context，
    // 保证连接生命周期内不跨线程迁移（亲和性），避免跨线程锁。
    boost::asio::io_context& get_io_context();

    std::size_t size() const noexcept { return contexts_.size(); }

private:
    std::vector<std::unique_ptr<boost::asio::io_context>> contexts_;
    // 每个 io_context 配一个 work guard，防止 run() 在无待处理任务时立即退出。
    std::vector<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> guards_;
    std::vector<std::thread> threads_;
    std::atomic<std::size_t> next_index_{0};
    std::atomic<bool> running_{false};
};

}  // namespace connection
}  // namespace gateway
}  // namespace cami
