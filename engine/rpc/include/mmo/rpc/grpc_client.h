// grpc_client.h — 统一 gRPC 客户端模板（TASK-006 §7 / §15.3 / §15.4）
//
// 红线：
//   - 禁止绕过本封装直接使用 grpc::Stub（engine/rpc 之外无 grpc:: 直接调用）；
//   - 非幂等调用重试次数 = 0；
//   - 每次重试携带**同一个** idempotency_key（一次 Call 生成一次）；
//   - 每次重试的每次尝试各自设置全新 deadline。
#pragma once

#include <cstdint>
#include <thread>
#include <functional>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <type_traits>
#include <utility>

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include "grpc_channel_pool.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/core/log/log_context.h"
#include "mmo/core/time/clock.h"
#include "mmo/core/uuid/uuid.h"
#include "rpc_metrics.h"
#include "rpc_options.h"
#include "status_mapping.h"

namespace mmo::rpc {

template <typename Stub>
class GrpcClient {
public:
    /// 由 channel 构造 stub 的工厂（如 EchoService::NewStub）。
    using StubFactory = std::function<std::unique_ptr<Stub>(std::shared_ptr<grpc::Channel>)>;

    GrpcClient(std::shared_ptr<GrpcChannelPool> pool, std::string target, StubFactory factory)
        : pool_(std::move(pool)), target_(std::move(target)), factory_(std::move(factory)) {}

    /// 同步调用封装。语义：
    ///   1. timeout_ms == 0 → INVALID_ARGUMENT（禁止永不超时）；
    ///   2. 每次尝试设置 deadline = now + timeout_ms；
    ///   3. metadata 携带 trace_id（取自线程 LogContext）与 idempotency_key；
    ///   4. 重试：仅 ShouldRetry() 为真且 attempts 有余量时，退避后重试，
    ///      每次重试带同一个 idempotency_key；
    ///   5. ctx_hook（可选）：调用方在每次尝试前拿到 ClientContext
    ///      （超时/取消测试与高级用法；禁止用它绕过封装建连）。
    template <typename Req, typename Resp>
    core::Result<Resp> Call(
        grpc::Status (Stub::*method)(grpc::ClientContext*, const Req&, Resp*),
        const Req& req, RpcOptions opts,
        const std::function<void(grpc::ClientContext&)>& ctx_hook = nullptr) {
        if (auto err = ValidateRpcOptions(opts); err.Code() != core::ErrorCode::OK) {
            return core::Result<Resp>::Fail(std::move(err));
        }
        auto channel_r = pool_->Get(target_);
        if (!channel_r.HasValue()) {
            return core::Result<Resp>::Fail(std::move(channel_r).Err());
        }
        auto stub = GetStub(channel_r.Value());
        if (!stub) {
            return core::Result<Resp>::Fail(core::Error{
                core::ErrorCode::INTERNAL_ERROR, "rpc: stub factory failed", core::domain::kNet});
        }

        const uint32_t max_attempts =
            ShouldRetry(opts, core::ErrorCode::BUSY) ? 1u + opts.max_retries : 1u;
        // 一次 Call 只生成一个 idempotency_key：所有重试尝试共用（§15.4）。
        // 非幂等调用无重试语义，跳过 UUID 生成（热路径零额外开销）。
        std::string idem_key;
        if (max_attempts > 1) idem_key = core::Uuid::NewV4().ToString();

        core::Error last_err{core::ErrorCode::INTERNAL_ERROR, "rpc: no attempt", core::domain::kNet};
        for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
            if (attempt > 0) {
                // 退避：指数 + 抖动 + 上限 1s（§19 重试风暴保护）。
                std::this_thread::sleep_for(
                    ComputeBackoff(attempt - 1, opts, static_cast<uint32_t>(idem_key.size() * 131u)));
            }
            const auto t0 = std::chrono::steady_clock::now();
            grpc::ClientContext ctx;
            SetDeadline(ctx, opts.timeout_ms);
            AttachMetadata(ctx, idem_key);
            if (ctx_hook) ctx_hook(ctx);

            Resp resp;
            const grpc::Status st = (stub->*method)(&ctx, req, &resp);
            const auto latency_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            const void* method_key = nullptr;
            std::memcpy(&method_key, &method, sizeof(method));
            RpcMetrics::Instance().Record(method_key, latency_us, st.ok(), attempt);

            if (st.ok()) return core::Result<Resp>::Ok(std::move(resp));
            last_err = MapStatus(st);
            if (!ShouldRetry(opts, last_err.Code())) break;
        }
        return core::Result<Resp>::Fail(std::move(last_err));
    }

    const std::string& target() const noexcept { return target_; }

private:
    static void SetDeadline(grpc::ClientContext& ctx, core::DurationMs timeout) {
        ctx.set_deadline(std::chrono::system_clock::now() + timeout);
    }

    /// trace_id：取线程 LogContext（TASK-002 体系），有效则透传；
    /// idempotency_key：所有尝试共用，服务端可据此去重。
    void AttachMetadata(grpc::ClientContext& ctx, const std::string& idem_key) const {
        const auto& lc = core::CurrentLogContext();
        if (lc.trace_id != core::kInvalidTraceId) {
            ctx.AddMetadata("trace_id", std::to_string(lc.trace_id));
        }
        if (!idem_key.empty()) ctx.AddMetadata("idempotency_key", idem_key);
    }

    /// stub 缓存：gRPC stub 对 unary 调用线程安全（const 方法），按 channel 复用，
    /// 避免每次调用重建（channel 本身由池复用，见 GrpcChannelPool）。
    Stub* GetStub(const std::shared_ptr<grpc::Channel>& ch) {
        std::lock_guard<std::mutex> lk(stub_mu_);
        auto it = stubs_.find(ch.get());
        if (it != stubs_.end()) return it->second.get();
        auto s = factory_(ch);
        if (!s) return nullptr;
        Stub* raw = s.get();
        stubs_.emplace(ch.get(), std::move(s));
        return raw;
    }

    std::shared_ptr<GrpcChannelPool> pool_;
    std::string target_;
    StubFactory factory_;
    std::mutex stub_mu_;
    std::unordered_map<const grpc::Channel*, std::unique_ptr<Stub>> stubs_;
};

}  // namespace mmo::rpc
