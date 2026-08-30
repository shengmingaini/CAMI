// echo_service.h — RPC 框架验证用回声服务实现（TASK-006 测试/bench 专用）
#pragma once

#include <grpcpp/grpcpp.h>

#include "service/echo.grpc.pb.h"

namespace mmo::rpc::testing {

/// 注入式回声服务：
///   - 同一 trace_hint 的调用次数计数（断言重试次数 / 非幂等只调一次）；
///   - fail_unavailable / fail_deadline：前 N 次注入对应失败；
///   - delay_ms：模拟慢请求；
///   - 回显客户端 metadata trace_id 与收集到的 idempotency_key。
class EchoServiceImpl final : public EchoService::Service {
public:
    grpc::Status Echo(grpc::ServerContext* ctx, const EchoRequest* req,
                      EchoResponse* resp) override;

    /// 该 trace_hint 已被服务端收到的调用次数（测试断言用）。
    static uint32_t CallsFor(int64_t trace_hint);

    /// 该 trace_hint 收到过的去重 idempotency_key 个数（断言重试共用同键）。
    static size_t KeysFor(int64_t trace_hint);

    static void ResetCounts();
};

}  // namespace mmo::rpc::testing
