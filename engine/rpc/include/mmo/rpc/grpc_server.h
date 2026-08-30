// grpc_server.h — 配置化 gRPC 服务端（TASK-006 §7 / §15.6 / §15.7）
//
// 任务书 §7 中 Config.max_recv_msg_size 标注为 DurationMs 属数据源笔误
//（消息大小是字节数），本实现取 size_t，字段名与默认值 4MB 不变。
// 已在 docs/INTERFACE.md 记录该偏差。
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"

namespace mmo::rpc {

/// 服务端令牌桶限流器（进程级；拦截器在其耗尽时返回 RESOURCE_EXHAUSTED）。
class RateLimiter {
public:
    /// rate_rps：每秒补充令牌数；burst：桶容量（瞬时放行上限）。
    RateLimiter(double rate_rps, uint32_t burst) noexcept;

    /// 取一枚令牌；桶空返回 false。
    bool TryAcquire() noexcept;

private:
    double rate_rps_;
    uint32_t burst_;
    double tokens_;
    std::chrono::steady_clock::time_point last_;
    std::mutex mu_;
};

class GrpcServer {
public:
    struct Config {
        std::string listen_addr;                 // 如 "0.0.0.0:50051"；端口 0 = 系统分配
        uint32_t max_threads{4};                 // 同步服务端 poller 线程上限
        size_t   max_recv_msg_size{4u * 1024u * 1024u};  // 最大接收消息字节数
        uint32_t rate_limit_rps{0};              // 0 = 关闭限流；>0 开启令牌桶（burst=rate）
    };

    /// 注册服务但尚未监听。listen_addr 为空 → INVALID_ARGUMENT。
    static core::Result<std::unique_ptr<GrpcServer>> Create(Config cfg,
                                                            std::vector<grpc::Service*> services);

    ~GrpcServer();

    /// 开始监听并启动 Wait 线程。绑定失败 → BUSY。
    core::Result<void> Start();

    /// 优雅关闭：grace 内等待在途请求完成，超时强制关闭。
    /// 幂等；重复调用直接返回。
    void Shutdown(core::DurationMs grace = std::chrono::milliseconds{5000});

    /// 实际绑定端口（listen_addr 端口为 0 时由系统分配；未 Start 前 = 0）。
    int bound_port() const noexcept { return bound_port_; }

    bool IsRunning() const noexcept;

private:
    GrpcServer() = default;

    std::unique_ptr<RateLimiter> limiter_;  // 拦截器持有裸指针，须先于 server_ 声明
    grpc::ServerBuilder pending_;
    int requested_port_{0};                 // Create 时端口占位（0 = 系统分配）
    std::unique_ptr<grpc::Server> server_;
    std::thread wait_thread_;
    int bound_port_{0};
    bool running_{false};
    std::mutex mu_;
};

}  // namespace mmo::rpc
