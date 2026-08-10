#include "gateway/connection/io_context_pool.h"

namespace cami {
namespace gateway {
namespace connection {

IoContextPool::IoContextPool(std::size_t pool_size)
    : contexts_(), guards_(), threads_() {
    if (pool_size == 0) pool_size = 1;
    contexts_.reserve(pool_size);
    for (std::size_t i = 0; i < pool_size; ++i) {
        auto ctx = std::make_unique<boost::asio::io_context>();
        guards_.push_back(boost::asio::make_work_guard(*ctx));
        contexts_.push_back(std::move(ctx));
    }
}

IoContextPool::~IoContextPool() {
    stop();
}

void IoContextPool::run() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;  // 已运行
    threads_.reserve(contexts_.size());
    for (auto& ctx : contexts_) {
        threads_.emplace_back([ctx_ptr = ctx.get()]() { ctx_ptr->run(); });
    }
}

void IoContextPool::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;  // 已停止/未运行
    for (auto& ctx : contexts_) {
        ctx->stop();
    }
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

boost::asio::io_context& IoContextPool::get_io_context() {
    // RR 取下一个 io_context；fetch_add 保证多线程并发建连分配安全。
    const std::size_t idx = next_index_.fetch_add(1, std::memory_order_relaxed) % contexts_.size();
    return *contexts_[idx];
}

}  // namespace connection
}  // namespace gateway
}  // namespace cami
