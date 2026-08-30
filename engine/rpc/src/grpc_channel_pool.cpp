// grpc_channel_pool.cpp — 连接池实现（TASK-006 §15.2）
#include "mmo/rpc/grpc_channel_pool.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/support/channel_arguments.h>

namespace mmo::rpc {

using core::Error;
using core::ErrorCode;
using core::Result;

Result<std::shared_ptr<GrpcChannelPool>> GrpcChannelPool::Create(size_t per_target) {
    if (per_target == 0) {
        return Result<std::shared_ptr<GrpcChannelPool>>::Fail(Error{
            ErrorCode::INVALID_ARGUMENT, "pool: per_target must be >= 1", core::domain::kNet});
    }
    return Result<std::shared_ptr<GrpcChannelPool>>::Ok(
        std::shared_ptr<GrpcChannelPool>(new GrpcChannelPool(per_target)));
}

GrpcChannelPool::GrpcChannelPool(size_t per_target) : per_target_(per_target) {}

GrpcChannelPool::~GrpcChannelPool() = default;

Result<std::shared_ptr<grpc::Channel>> GrpcChannelPool::Get(std::string_view target) {
    if (target.empty()) {
        return Result<std::shared_ptr<grpc::Channel>>::Fail(
            Error{ErrorCode::INVALID_ARGUMENT, "pool: empty target", core::domain::kNet});
    }
    std::lock_guard<std::mutex> lk(mu_);
    auto& e = by_target_[std::string(target)];
    if (e.channels.size() < per_target_) {
        // 逐个补足池容量（首次 Get 只建 1 条，后续 Get 补满，避免冷启动风暴）。
        // 内部 RPC 不走系统/环境 HTTP 代理（企业代理会把回环 502）。
        grpc::ChannelArguments args;
        args.SetInt("grpc.enable_http_proxy", 0);
        e.channels.push_back(grpc::CreateCustomChannel(
            std::string(target), grpc::InsecureChannelCredentials(), args));
        e.next = static_cast<uint32_t>(e.channels.size()) - 1;
        return Result<std::shared_ptr<grpc::Channel>>::Ok(e.channels.back());
    }
    // 空闲健康检查：TRANSIENT_FAILURE 触发有界重连探测（100ms 内连不上
    // 交由 gRPC 内部退避，本调用不阻塞业务）。
    auto& ch = e.channels[e.next % per_target_];
    if (ch->GetState(false) == GRPC_CHANNEL_TRANSIENT_FAILURE) {
        (void)ch->WaitForConnected(std::chrono::system_clock::now() + std::chrono::milliseconds{100});
    }
    auto out = ch;
    e.next = (e.next + 1) % per_target_;
    return Result<std::shared_ptr<grpc::Channel>>::Ok(std::move(out));
}

void GrpcChannelPool::HealthCheck() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [target, e] : by_target_) {
        for (auto& ch : e.channels) {
            if (ch->GetState(false) == GRPC_CHANNEL_TRANSIENT_FAILURE) {
                (void)ch->WaitForConnected(std::chrono::system_clock::now() +
                                           std::chrono::milliseconds{100});
            } else if (ch->GetState(false) == GRPC_CHANNEL_SHUTDOWN) {
                grpc::ChannelArguments args;
                args.SetInt("grpc.enable_http_proxy", 0);
                ch = grpc::CreateCustomChannel(
                    target, grpc::InsecureChannelCredentials(), args);
            }
        }
    }
}

size_t GrpcChannelPool::TargetCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return by_target_.size();
}

}  // namespace mmo::rpc
