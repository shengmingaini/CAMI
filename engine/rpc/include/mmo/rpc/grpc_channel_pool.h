// grpc_channel_pool.h — gRPC Channel 连接池（TASK-006 §4 / §7）
//
// 状态归属：channel 生命周期（创建 / 复用 / 重连 / 健康检查）由本池独占维护。
// 调用方只拿 shared_ptr<Channel>，**禁止自行 grpc::CreateChannel**。
// 红线：禁止每次调用新建 channel。
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <grpcpp/channel.h>

#include "mmo/core/error/result.h"

namespace mmo::rpc {

class GrpcChannelPool {
public:
    /// per_target：同一 target 池内维持的 channel 数（轮询分发）。
    /// 必须 >= 1，否则返回 INVALID_ARGUMENT。
    static core::Result<std::shared_ptr<GrpcChannelPool>> Create(size_t per_target = 4);

    ~GrpcChannelPool();
    GrpcChannelPool(const GrpcChannelPool&) = delete;
    GrpcChannelPool& operator=(const GrpcChannelPool&) = delete;

    /// 取 target 的 channel（同 target 内轮询）。target 为空 → INVALID_ARGUMENT。
    /// channel 处于 TRANSIENT_FAILURE 时触发 TryToReconnect（空闲重连）。
    core::Result<std::shared_ptr<grpc::Channel>> Get(std::string_view target);

    /// 全量健康检查：对处于 TRANSIENT_FAILURE / SHUTDOWN 的 channel
    /// 触发重连；SHUTDOWN 的 channel 直接重建。供后台定时任务调用。
    void HealthCheck();

    /// 池内 target 数（测试/审计用）。
    size_t TargetCount() const;

private:
    struct Entry {
        std::vector<std::shared_ptr<grpc::Channel>> channels;
        uint32_t next{0};  // 轮询游标
    };

    explicit GrpcChannelPool(size_t per_target);

    size_t per_target_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> by_target_;
};

}  // namespace mmo::rpc
