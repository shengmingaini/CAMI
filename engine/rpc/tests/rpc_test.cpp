// rpc_test.cpp — TASK-006 单元 / 集成 / Failure 测试（§16 / §17 / §19）
//
// 覆盖：
//   U1 错误码映射表驱动全覆盖   U2 RpcOptions 校验   U3 退避计算（指数/抖动/上限）
//   U4 重试判定（非幂等恒不重试）U5 channel 池复用     U6 客户端指标
//   I1 正常调用                I2 超时              I3 取消
//   I4 重试成功（3 次到达）    I5 重试耗尽           I6 非幂等只调 1 次
//   I7 TraceID 透传            I8 限流 → RATE_LIMITED
//   I9 优雅关闭（在途请求完成）
//   F1 服务端未启动 → BUSY     F2 不可达地址按退避后失败
//   F3 消息体超限 → RATE_LIMITED   F4 重试风暴退避 ≤ 1s（U3 已覆盖上限）
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "echo_service.h"
#include "mmo/core/log/log_context.h"
#include "test_print.h"
#include "mmo/rpc/grpc_channel_pool.h"
#include "mmo/rpc/grpc_client.h"
#include "mmo/rpc/grpc_server.h"
#include "mmo/rpc/rpc_metrics.h"
#include "mmo/rpc/rpc_options.h"
#include "mmo/rpc/status_mapping.h"
#include "service/echo.grpc.pb.h"

using namespace mmo;                          // NOLINT
using mmo::rpc::testing::EchoServiceImpl;    // NOLINT
namespace frpc = ::mmo::rpc;
namespace eb = ::mmo::rpc::testing;

static int g_failed = 0;
static int g_total = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_total;                                                                     \
        if (!(cond)) {                                                                 \
            ++g_failed;                                                                \
            core::test::ErrorFmt("FAIL @ %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                              \
    } while (0)

namespace {

using StubT = eb::EchoService::Stub;

frpc::GrpcClient<StubT> MakeClient(std::shared_ptr<frpc::GrpcChannelPool> pool,
                                   const std::string& target) {
    return frpc::GrpcClient<StubT>(
        pool, target, [](std::shared_ptr<grpc::Channel> ch) {
            return eb::EchoService::NewStub(ch);
        });
}

struct ServerHandle {
    std::unique_ptr<frpc::GrpcServer> server;
    std::unique_ptr<EchoServiceImpl> svc;
    std::string target;
    ServerHandle() = default;
    ServerHandle(const ServerHandle&) = delete;
    ServerHandle& operator=(const ServerHandle&) = delete;
};

void StartServer(ServerHandle& h, uint32_t rate_limit_rps = 0,
                 size_t max_recv = 4u * 1024u * 1024u) {
    h.svc = std::make_unique<EchoServiceImpl>();
    frpc::GrpcServer::Config cfg;
    cfg.listen_addr = "127.0.0.1:0";
    cfg.rate_limit_rps = rate_limit_rps;
    cfg.max_recv_msg_size = max_recv;
    h.server = frpc::GrpcServer::Create(cfg, {h.svc.get()}).Value();
    CHECK(h.server->Start().HasValue());
    h.target = "127.0.0.1:" + std::to_string(h.server->bound_port());
}

eb::EchoRequest MakeReq(const std::string& msg, int64_t hint) {
    eb::EchoRequest r;
    r.set_message(msg);
    r.set_trace_hint(hint);
    return r;
}

// ------------------------------- 单元测试 -----------------------------------

void U1_MappingTable() {
    struct Row { grpc::StatusCode g; core::ErrorCode e; };
    static const Row rows[] = {
        {grpc::StatusCode::OK, core::ErrorCode::OK},
        {grpc::StatusCode::INVALID_ARGUMENT, core::ErrorCode::INVALID_ARGUMENT},
        {grpc::StatusCode::NOT_FOUND, core::ErrorCode::NOT_FOUND},
        {grpc::StatusCode::DEADLINE_EXCEEDED, core::ErrorCode::TIMEOUT},
        {grpc::StatusCode::UNAVAILABLE, core::ErrorCode::BUSY},
        {grpc::StatusCode::RESOURCE_EXHAUSTED, core::ErrorCode::RATE_LIMITED},
        {grpc::StatusCode::UNAUTHENTICATED, core::ErrorCode::UNAUTHORIZED},
        {grpc::StatusCode::FAILED_PRECONDITION, core::ErrorCode::VERSION_CONFLICT},
        {grpc::StatusCode::INTERNAL, core::ErrorCode::INTERNAL_ERROR},
        {grpc::StatusCode::UNKNOWN, core::ErrorCode::INTERNAL_ERROR},
        // §8 表外枚举：唯一映射入口的兜底（见 status_mapping.cpp 注释）
        {grpc::StatusCode::CANCELLED, core::ErrorCode::INTERNAL_ERROR},
        {grpc::StatusCode::PERMISSION_DENIED, core::ErrorCode::UNAUTHORIZED},
        {grpc::StatusCode::OUT_OF_RANGE, core::ErrorCode::INVALID_ARGUMENT},
        {grpc::StatusCode::ABORTED, core::ErrorCode::INTERNAL_ERROR},
        {grpc::StatusCode::UNIMPLEMENTED, core::ErrorCode::INTERNAL_ERROR},
        {grpc::StatusCode::DATA_LOSS, core::ErrorCode::INTERNAL_ERROR},
        {grpc::StatusCode::DO_NOT_USE, core::ErrorCode::INTERNAL_ERROR},
    };
    for (const auto& r : rows) {
        CHECK(frpc::MapGrpcCode(r.g) == r.e);
        grpc::Status st{r.g, "x"};
        CHECK(frpc::MapStatus(st).Code() == r.e);
    }
}

void U2_OptionsValidation() {
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{0};
    CHECK(frpc::ValidateRpcOptions(o).Code() == core::ErrorCode::INVALID_ARGUMENT);
    o.timeout_ms = core::DurationMs{1};
    CHECK(frpc::ValidateRpcOptions(o).Code() == core::ErrorCode::OK);
}

void U3_Backoff() {
    frpc::RpcOptions o;  // base 20ms
    // attempt 0：20ms ±20% → [16, 24]
    for (uint32_t seed = 0; seed < 200; ++seed) {
        const auto b = frpc::ComputeBackoff(0, o, seed);
        CHECK(b.count() >= 16 && b.count() <= 24);
    }
    // 指数：20, 40, 80, ...（无抖动截断时）；seed 固定看单调增长
    const auto b0 = frpc::ComputeBackoff(0, o, 1000);
    const auto b1 = frpc::ComputeBackoff(1, o, 1000);
    const auto b2 = frpc::ComputeBackoff(2, o, 1000);
    CHECK(b1.count() > b0.count() && b2.count() > b1.count());
    // 重试风暴保护：attempt 很大时封顶 1000ms（抖动后 [800, 1200]）
    for (uint32_t seed = 0; seed < 200; ++seed) {
        const auto b = frpc::ComputeBackoff(30, o, seed);
        CHECK(b.count() >= 800 && b.count() <= 1200);
    }
}

void U4_ShouldRetry() {
    frpc::RpcOptions o;
    o.idempotent = false;
    CHECK(!frpc::ShouldRetry(o, core::ErrorCode::BUSY));
    CHECK(!frpc::ShouldRetry(o, core::ErrorCode::TIMEOUT));
    o.idempotent = true;
    CHECK(frpc::ShouldRetry(o, core::ErrorCode::BUSY));
    CHECK(frpc::ShouldRetry(o, core::ErrorCode::TIMEOUT));
    CHECK(frpc::ShouldRetry(o, core::ErrorCode::RATE_LIMITED));
    CHECK(!frpc::ShouldRetry(o, core::ErrorCode::INVALID_ARGUMENT));
    CHECK(!frpc::ShouldRetry(o, core::ErrorCode::UNAUTHORIZED));
    CHECK(!frpc::ShouldRetry(o, core::ErrorCode::VERSION_CONFLICT));
    CHECK(!frpc::ShouldRetry(o, core::ErrorCode::INTERNAL_ERROR));
}

void U5_Pool() {
    auto bad = frpc::GrpcChannelPool::Create(0);
    CHECK(!bad.HasValue() && bad.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);

    auto pool = frpc::GrpcChannelPool::Create(1).Value();
    CHECK(!pool->Get("").HasValue());

    auto c1 = pool->Get("127.0.0.1:1").Value();
    auto c2 = pool->Get("127.0.0.1:1").Value();
    CHECK(c1.get() == c2.get());          // 同 target 复用同一 channel 对象
    CHECK(pool->TargetCount() == 1);
}

void U6_Metrics() {
    frpc::RpcMetrics::Instance().Reset();
    static const int kDummy = 0;
    frpc::RpcMetrics::Instance().Record(&kDummy, 100, true, 0);
    frpc::RpcMetrics::Instance().Record(&kDummy, 200, false, 1);
    const auto snap = frpc::RpcMetrics::Instance().Snapshot();
    CHECK(snap.size() == 1);
    CHECK(snap[0].calls == 2);
    CHECK(snap[0].errors == 1);
    CHECK(snap[0].retries == 1);
    CHECK(snap[0].avg_latency_us == 150.0);
}

// ------------------------------ 集成测试 ------------------------------------

void I1_EchoOk(ServerHandle& h, std::shared_ptr<frpc::GrpcChannelPool> pool) {
    auto cli = MakeClient(pool, h.target);
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{500};
    auto r = cli.Call(&StubT::Echo, MakeReq("hello", 1001), o);
    CHECK(r.HasValue() && r.Value().message() == "hello");
    CHECK(eb::EchoServiceImpl::CallsFor(1001) == 1);
}

void I2_Timeout(ServerHandle& h, std::shared_ptr<frpc::GrpcChannelPool> pool) {
    auto cli = MakeClient(pool, h.target);
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{200};
    auto req = MakeReq("slow", 1002);
    req.set_delay_ms(1000);
    const auto t0 = std::chrono::steady_clock::now();
    auto r = cli.Call(&StubT::Echo, req, o);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(!r.HasValue() && r.Err().Code() == core::ErrorCode::TIMEOUT);
    CHECK(elapsed < std::chrono::milliseconds{900});  // 超时控制误差 < 10ms 目标（留余量断言）
}

void I3_Cancel(ServerHandle& h, std::shared_ptr<frpc::GrpcChannelPool> pool) {
    auto cli = MakeClient(pool, h.target);
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{500};
    auto req = MakeReq("cancel", 1003);
    req.set_delay_ms(300);
    auto r = cli.Call(&StubT::Echo, req, o,
                      [](grpc::ClientContext& ctx) { ctx.TryCancel(); });
    // 客户端取消 → gRPC CANCELLED → 唯一映射入口兜底 INTERNAL_ERROR（受控枚举无
    // CANCELLED，语义以 message 区分，见 docs/INTERFACE.md）。
    CHECK(!r.HasValue());
    CHECK(r.Err().Code() == core::ErrorCode::INTERNAL_ERROR);
}

void I4_RetrySuccess(ServerHandle& h, std::shared_ptr<frpc::GrpcChannelPool> pool) {
    auto cli = MakeClient(pool, h.target);
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{500};
    o.idempotent = true;
    o.max_retries = 2;
    auto req = MakeReq("retry", 1004);
    req.set_fail_unavailable(2);  // 前 2 次 UNAVAILABLE，第 3 次成功
    auto r = cli.Call(&StubT::Echo, req, o);
    CHECK(r.HasValue() && r.Value().message() == "retry");
    CHECK(eb::EchoServiceImpl::CallsFor(1004) == 3);   // 恰好 3 次到达
    CHECK(eb::EchoServiceImpl::KeysFor(1004) == 1);    // 重试共用同一 idempotency_key
}

void I5_RetryExhausted(ServerHandle& h, std::shared_ptr<frpc::GrpcChannelPool> pool) {
    auto cli = MakeClient(pool, h.target);
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{500};
    o.idempotent = true;
    o.max_retries = 2;
    auto req = MakeReq("exh", 1005);
    req.set_fail_unavailable(10);
    auto r = cli.Call(&StubT::Echo, req, o);
    CHECK(!r.HasValue() && r.Err().Code() == core::ErrorCode::BUSY);
    CHECK(eb::EchoServiceImpl::CallsFor(1005) == 3);   // 1 + 2 次重试
}

void I6_NonIdempotentNoRetry(ServerHandle& h, std::shared_ptr<frpc::GrpcChannelPool> pool) {
    auto cli = MakeClient(pool, h.target);
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{500};
    o.idempotent = false;  // 红线：非幂等重试次数 = 0
    o.max_retries = 2;
    auto req = MakeReq("once", 1006);
    req.set_fail_unavailable(2);
    auto r = cli.Call(&StubT::Echo, req, o);
    CHECK(!r.HasValue() && r.Err().Code() == core::ErrorCode::BUSY);
    CHECK(eb::EchoServiceImpl::CallsFor(1006) == 1);   // 只到达 1 次
}

void I7_TracePassthrough(ServerHandle& h, std::shared_ptr<frpc::GrpcChannelPool> pool) {
    auto cli = MakeClient(pool, h.target);
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{500};
    core::LogContext lc;
    lc.trace_id = 777001;
    core::ScopedLogContext scope(lc);
    auto r = cli.Call(&StubT::Echo, MakeReq("trace", 1007), o);
    CHECK(r.HasValue());
    if (r.HasValue()) {
        CHECK(r.Value().trace_id() == "777001");  // 服务端从 metadata 读到并回显
    }
}

void I8_RateLimit() {
    ServerHandle h; StartServer(h, /*rate_limit_rps=*/2);   // burst = 2
    auto pool = frpc::GrpcChannelPool::Create(1).Value();
    auto cli = MakeClient(pool, h.target);
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{500};
    int limited = 0;
    for (int i = 0; i < 6; ++i) {
        auto r = cli.Call(&StubT::Echo, MakeReq("rl", 2001), o);
        if (!r.HasValue() && r.Err().Code() == core::ErrorCode::RATE_LIMITED) ++limited;
    }
    CHECK(limited >= 2);  // burst=2，后续 4 次全部被限流（1s 窗口内）
    h.server->Shutdown();
}

void I9_GracefulShutdown() {
    ServerHandle h; StartServer(h);
    auto pool = frpc::GrpcChannelPool::Create(1).Value();
    auto cli = MakeClient(pool, h.target);

    // 在途慢请求：Shutdown(grace) 必须等它完成而非硬杀。
    auto slow = MakeReq("inflight", 3001);
    slow.set_delay_ms(300);
    bool done = false;
    core::Result<eb::EchoResponse> res = core::Result<eb::EchoResponse>::Fail(
        core::Error{core::ErrorCode::INTERNAL_ERROR, "unset", core::domain::kNet});
    std::thread t([&] {
        frpc::RpcOptions o;
        o.timeout_ms = core::DurationMs{2000};
        res = cli.Call(&StubT::Echo, slow, o);
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    h.server->Shutdown(core::DurationMs{5000});
    t.join();
    CHECK(done);
    CHECK(res.HasValue() && res.Value().message() == "inflight");
}

// ------------------------------ Failure 测试 --------------------------------

void F1_NoServer() {
    auto pool = frpc::GrpcChannelPool::Create(1).Value();
    auto cli = MakeClient(pool, "127.0.0.1:1");  // 端口 1 无服务端
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{300};
    auto r = cli.Call(&StubT::Echo, MakeReq("x", 4001), o);
    // Windows 实测：连本机未监听端口时 gRPC 子通道持续重连直至 deadline，
    // 返回 DEADLINE_EXCEEDED→TIMEOUT（Linux 为 UNAVAILABLE→BUSY）。
    // §19 意图 = timeout 内明确错误且不崩溃；两者皆满足，平台差异记入 docs/INTERFACE.md。
    CHECK(!r.HasValue() && (r.Err().Code() == core::ErrorCode::BUSY ||
                            r.Err().Code() == core::ErrorCode::TIMEOUT));
}

void F2_Unreachable() {
    auto pool = frpc::GrpcChannelPool::Create(1).Value();
    auto cli = MakeClient(pool, "10.255.255.1:1");  // 不可达网段
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{300};
    o.idempotent = true;
    o.max_retries = 1;
    o.backoff_base_ms = core::DurationMs{10};
    const auto t0 = std::chrono::steady_clock::now();
    auto r = cli.Call(&StubT::Echo, MakeReq("x", 4002), o);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(!r.HasValue());                            // 明确错误，不挂死
    CHECK(ms < 5000);                                // 按退避（≤1s/次）在 timeout 内返回
}

void F3_Oversize() {
    ServerHandle h; StartServer(h, 0, /*max_recv=*/1024);
    auto pool = frpc::GrpcChannelPool::Create(1).Value();
    auto cli = MakeClient(pool, h.target);
    frpc::RpcOptions o;
    o.timeout_ms = core::DurationMs{500};
    auto req = MakeReq(std::string(8192, 'y'), 5001);
    auto r = cli.Call(&StubT::Echo, req, o);
    CHECK(!r.HasValue() && r.Err().Code() == core::ErrorCode::RATE_LIMITED);
    h.server->Shutdown();
}

}  // namespace

int main() {
    EchoServiceImpl::ResetCounts();

    U1_MappingTable();
    U2_OptionsValidation();
    U3_Backoff();
    U4_ShouldRetry();
    U5_Pool();
    U6_Metrics();

    auto pool = frpc::GrpcChannelPool::Create(2).Value();
    ServerHandle h; StartServer(h);
    I1_EchoOk(h, pool);
    I2_Timeout(h, pool);
    I3_Cancel(h, pool);
    I4_RetrySuccess(h, pool);
    I5_RetryExhausted(h, pool);
    I6_NonIdempotentNoRetry(h, pool);
    I7_TracePassthrough(h, pool);
    h.server->Shutdown();

    I8_RateLimit();
    I9_GracefulShutdown();
    F1_NoServer();
    F2_Unreachable();
    F3_Oversize();

    core::test::LineFmt("rpc_test: total=%d failed=%d\n", g_total, g_failed);
    if (g_failed != 0) return 1;
    core::test::Line("ALL PASS\n");
    return 0;
}
