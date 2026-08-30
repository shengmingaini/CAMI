// rpc_bench.cpp — TASK-006 §18 benchmark：本机回环 Echo
//
// 输出（bench/rpc.txt，key=value，验收脚本断言 rpc_p99_us <= 1000）：
//   rpc_p50_us / rpc_p99_us / rpc_qps / wrapper_overhead_us / timeout_overhead_us / iterations
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include "echo_service.h"
#include "test_print.h"
#include "mmo/rpc/grpc_channel_pool.h"
#include "mmo/rpc/grpc_client.h"
#include "mmo/rpc/grpc_server.h"
#include "service/echo.grpc.pb.h"

using namespace mmo;  // NOLINT
namespace frpc = ::mmo::rpc;
namespace eb = ::mmo::rpc::testing;

namespace {

using StubT = eb::EchoService::Stub;

uint64_t PercentileUs(std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
    return v[idx];
}

}  // namespace

int main(int argc, char** argv) {
    long long iterations = 100000;
    if (argc > 2 && std::string(argv[1]) == "--iterations") iterations = atoll(argv[2]);
    if (iterations < 1000) iterations = 1000;

    eb::EchoServiceImpl svc;
    frpc::GrpcServer::Config cfg;
    cfg.listen_addr = "127.0.0.1:0";
    auto server = frpc::GrpcServer::Create(cfg, {&svc}).Value();
    if (!server->Start().HasValue()) {
        core::test::Error("bench: server start failed\n");
        return 1;
    }
    const std::string target = "127.0.0.1:" + std::to_string(server->bound_port());
    auto pool = frpc::GrpcChannelPool::Create(1).Value();
    auto cli = frpc::GrpcClient<StubT>(pool, target, [](std::shared_ptr<grpc::Channel> ch) {
        return eb::EchoService::NewStub(ch);
    });

    // 裸 stub 基线（同 channel 同 target，量化封装开销）。
    auto raw_ch = pool->Get(target).Value();
    auto raw_stub = eb::EchoService::NewStub(raw_ch);

    eb::EchoRequest req;
    req.set_message("bench-payload-64B-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    req.set_trace_hint(999999);

    frpc::RpcOptions opts;
    opts.timeout_ms = std::chrono::milliseconds{1000};

    // 预热
    for (int i = 0; i < 1000; ++i) {
        (void)cli.Call(&StubT::Echo, req, opts);
    }

    std::vector<uint64_t> wrap_us;
    std::vector<uint64_t> raw_us;
    wrap_us.reserve(static_cast<size_t>(iterations));
    const auto t0 = std::chrono::steady_clock::now();
    for (long long i = 0; i < iterations; ++i) {
        const auto s = std::chrono::steady_clock::now();
        auto r = cli.Call(&StubT::Echo, req, opts);
        const auto e = std::chrono::steady_clock::now();
        if (!r.HasValue()) {
            core::test::Error("bench: call failed\n");
            return 1;
        }
        wrap_us.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(e - s).count()));
    }
    const double total_sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // 裸 stub 对照（1/10 规模足够估 p50）。
    for (long long i = 0; i < iterations / 10; ++i) {
        const auto s = std::chrono::steady_clock::now();
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds{1000});
        eb::EchoResponse resp;
        auto st = raw_stub->Echo(&ctx, req, &resp);
        const auto e = std::chrono::steady_clock::now();
        if (!st.ok()) {
            core::test::Error("bench: raw call failed\n");
            return 1;
        }
        raw_us.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(e - s).count()));
    }

    const uint64_t p50 = PercentileUs(wrap_us, 0.50);
    const uint64_t p99 = PercentileUs(wrap_us, 0.99);
    const uint64_t raw_p50 = PercentileUs(raw_us, 0.50);
    const double qps = static_cast<double>(iterations) / total_sec;
    const long long wrapper_overhead = p50 > raw_p50 ? static_cast<long long>(p50 - raw_p50) : 0;
    // 超时控制开销：同调用把 deadline 从 1s 收紧到 100ms 后的 p50 变化。
    std::vector<uint64_t> tight_us;
    frpc::RpcOptions tight = opts;
    tight.timeout_ms = std::chrono::milliseconds{100};
    for (int i = 0; i < 1000; ++i) {
        const auto s = std::chrono::steady_clock::now();
        auto r = cli.Call(&StubT::Echo, req, tight);
        const auto e = std::chrono::steady_clock::now();
        if (!r.HasValue()) return 1;
        tight_us.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(e - s).count()));
    }
    const uint64_t tight_p50 = PercentileUs(tight_us, 0.50);
    const long long timeout_overhead =
        tight_p50 > raw_p50 ? static_cast<long long>(tight_p50 - raw_p50) : 0;

    const auto write_kv = [&](const char* k, auto v) {
        core::test::LineFmt("%s=%lld\n", k, static_cast<long long>(v));
    };
    write_kv("rpc_p50_us", p50);
    write_kv("rpc_p99_us", p99);
    core::test::LineFmt("rpc_qps=%.0f\n", qps);
    write_kv("wrapper_overhead_us", wrapper_overhead);
    write_kv("timeout_overhead_us", timeout_overhead);
    write_kv("iterations", iterations);

    FILE* f = fopen("bench/rpc.txt", "w");
    if (f) {
        fprintf(f, "rpc_p50_us=%llu\n", static_cast<unsigned long long>(p50));
        fprintf(f, "rpc_p99_us=%llu\n", static_cast<unsigned long long>(p99));
        fprintf(f, "rpc_qps=%.0f\n", qps);
        fprintf(f, "wrapper_overhead_us=%lld\n", wrapper_overhead);
        fprintf(f, "timeout_overhead_us=%lld\n", timeout_overhead);
        fprintf(f, "iterations=%lld\n", iterations);
        fclose(f);
    }

    server->Shutdown();
    return 0;
}
