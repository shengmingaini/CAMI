// grpc_server.cpp — 服务端实现 + 三个拦截器（TASK-006 §15.6 / §15.7）
#include "mmo/rpc/grpc_server.h"

#include <chrono>

#include <grpcpp/support/interceptor.h>
#include <grpcpp/support/server_interceptor.h>

#include "mmo/core/log/logger.h"

namespace mmo::rpc {

using core::Error;
using core::ErrorCode;
using core::Result;

// ---------------------------------------------------------------------------
// RateLimiter（令牌桶）
// ---------------------------------------------------------------------------
RateLimiter::RateLimiter(double rate_rps, uint32_t burst) noexcept
    : rate_rps_(rate_rps), burst_(burst), tokens_(static_cast<double>(burst)),
      last_(std::chrono::steady_clock::now()) {}

bool RateLimiter::TryAcquire() noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    tokens_ = std::min(static_cast<double>(burst_), tokens_ + secs * rate_rps_);
    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 拦截器：TraceID 透传 + 日志 + 限流（§15.7）
//
// 服务端同步 API 下钩子语义：
//   POST_RECV_INITIAL_METADATA：已收到客户端 metadata（trace_id 在此提取）；
//   PRE_SEND_STATUS：即将回发终态 → 限流拒绝在此用 ModifySendStatus 改写。
// ---------------------------------------------------------------------------
namespace {

constexpr char kModule[] = "rpc";

class RpcServerInterceptor final : public grpc::experimental::Interceptor {
public:
    explicit RpcServerInterceptor(grpc::experimental::ServerRpcInfo* info,
                                  RateLimiter* limiter)
        : info_(info), limiter_(limiter),
          start_(std::chrono::steady_clock::now()) {}

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
        if (methods->QueryInterceptionHookPoint(
                grpc::experimental::InterceptionHookPoints::POST_RECV_INITIAL_METADATA)) {
            // TraceID 透传：提取客户端 metadata trace_id（记录传播，不改写）。
            if (auto* md = methods->GetRecvInitialMetadata()) {
                if (auto it = md->find("trace_id"); it != md->end()) {
                    trace_id_.assign(it->second.begin(), it->second.end());
                }
            }
            // 限流：耗尽则标记，PRE_SEND_STATUS 改写终态（§15.7）。
            if (limiter_ && !limiter_->TryAcquire()) limited_ = true;
        }
        if (methods->QueryInterceptionHookPoint(
                grpc::experimental::InterceptionHookPoints::PRE_SEND_STATUS)) {
            const auto latency_us = static_cast<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start_).count());
            grpc::Status st = methods->GetSendStatus();
            if (limited_) {
                st = grpc::Status{grpc::StatusCode::RESOURCE_EXHAUSTED, "rate limited"};
                methods->ModifySendStatus(st);
            }
            // 日志拦截器：method / latency / status / trace_id。
            if (core::Logger::ShouldLog(core::LogLevel::Info)) {
                core::LogContext ctx;
                ctx.module = kModule;
                core::Logger::Write(core::LogLevel::Info, ctx,
                                    "rpc method={} status={} latency_us={} trace_id={}",
                                    info_->method(), static_cast<int>(st.error_code()),
                                    latency_us, trace_id_.empty() ? "-" : trace_id_);
            }
        }
        methods->Proceed();
    }

private:
    grpc::experimental::ServerRpcInfo* info_;
    RateLimiter* limiter_;
    std::chrono::steady_clock::time_point start_;
    std::string trace_id_;
    bool limited_{false};
};

class RpcInterceptorFactory final : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    explicit RpcInterceptorFactory(RateLimiter* limiter) : limiter_(limiter) {}
    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override {
        return new RpcServerInterceptor(info, limiter_);
    }

private:
    RateLimiter* limiter_;
};

}  // namespace

// ---------------------------------------------------------------------------
// GrpcServer
// ---------------------------------------------------------------------------
GrpcServer::~GrpcServer() { Shutdown(); }

Result<std::unique_ptr<GrpcServer>> GrpcServer::Create(Config cfg,
                                                       std::vector<grpc::Service*> services) {
    if (cfg.listen_addr.empty()) {
        return Result<std::unique_ptr<GrpcServer>>::Fail(
            Error{ErrorCode::INVALID_ARGUMENT, "server: empty listen_addr", core::domain::kNet});
    }
    if (services.empty()) {
        return Result<std::unique_ptr<GrpcServer>>::Fail(
            Error{ErrorCode::INVALID_ARGUMENT, "server: no service registered", core::domain::kNet});
    }

    auto srv = std::unique_ptr<GrpcServer>(new GrpcServer());
    if (cfg.rate_limit_rps > 0) {
        srv->limiter_ = std::make_unique<RateLimiter>(static_cast<double>(cfg.rate_limit_rps),
                                                      cfg.rate_limit_rps);
    }

    // 端口回填发生在 BuildAndStart()（Start 阶段），必须直接指向成员变量。
    grpc::ServerBuilder& builder = srv->pending_;
    builder.AddListeningPort(cfg.listen_addr, grpc::InsecureServerCredentials(),
                             &srv->requested_port_);
    for (auto* svc : services) builder.RegisterService(svc);
    builder.SetMaxReceiveMessageSize(static_cast<int>(cfg.max_recv_msg_size));
    builder.SetSyncServerOption(grpc::ServerBuilder::NUM_CQS, 1);
    builder.SetSyncServerOption(grpc::ServerBuilder::MIN_POLLERS, 1);
    builder.SetSyncServerOption(grpc::ServerBuilder::MAX_POLLERS,
                                static_cast<int>(cfg.max_threads));

    std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> factories;
    factories.push_back(std::make_unique<RpcInterceptorFactory>(srv->limiter_.get()));
    builder.experimental().SetInterceptorCreators(std::move(factories));

    return Result<std::unique_ptr<GrpcServer>>::Ok(std::move(srv));
}

core::Result<void> GrpcServer::Start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return core::Result<void>::Ok();
    server_ = std::move(pending_).BuildAndStart();
    if (!server_) {
        return core::Result<void>::Fail(
            Error{ErrorCode::BUSY, "server: bind/listen failed", core::domain::kNet});
    }
    bound_port_ = requested_port_;
    running_ = true;
    wait_thread_ = std::thread([this] { server_->Wait(); });
    return core::Result<void>::Ok();
}

void GrpcServer::Shutdown(core::DurationMs grace) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!running_) return;
    running_ = false;
    // grace 内等待在途请求完成；到期强制关闭（gRPC 内部处理，Wait 线程自然退出）。
    server_->Shutdown(std::chrono::system_clock::now() + grace);
    lk.unlock();
    if (wait_thread_.joinable()) wait_thread_.join();
    if (core::Logger::ShouldLog(core::LogLevel::Info)) {
        core::LogContext ctx;
        ctx.module = kModule;
        core::Logger::Write(core::LogLevel::Info, ctx, "rpc server shutdown grace_ms={}",
                            static_cast<long long>(grace.count()));
    }
}

bool GrpcServer::IsRunning() const noexcept { return running_; }

}  // namespace mmo::rpc
