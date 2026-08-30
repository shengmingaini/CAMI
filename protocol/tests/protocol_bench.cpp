// protocol/tests/protocol_bench.cpp — TASK-005 编解码 Benchmark
//
// 输出机器可读 key=value 到 stdout 与 bench/protocol.txt（验收脚本 assert_metric 解析）。
// 指标（任务书 §18 / §22）：
//   pb_encode_ns / pb_decode_ns  —— Protobuf 小消息每操作纳秒（目标 < 2000）
//   fbs_encode_ns / fbs_decode_ns —— FlatBuffers 每操作纳秒（解码目标 <= 300）
//   alloc_per_op                  —— Protobuf 解码平均每次堆分配次数
//   fbs_decode_allocs             —— FlatBuffers 解码总堆分配次数（目标 = 0，零拷贝）
//
// 分配计数：全局 operator new/delete 重载（含 array / aligned 变体），
// 计数窗口内运行的解码循环若触碰堆即会计入。
//
// 反优化：每次解码结果的关键指针/长度落到 volatile sink，防止 -O3 消除循环体。

#include "mmo/protocol/codec/flatbuf_codec.h"
#include "mmo/protocol/codec/protobuf_codec.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "test_print.h"

namespace {

namespace tprint = ::mmo::core::test;
using mmo::protocol::EnvelopeMessageType;
using mmo::protocol::EnvelopeView;

std::atomic<long long> g_allocs{0};

}  // namespace

// ---- 全局分配计数（替换全局 operator new/delete） --------------------------
void* operator new(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) { std::abort(); }
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return p;
}
void* operator new[](std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) { std::abort(); }
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#if defined(__cpp_sized_deallocation) || __cplusplus >= 201703L
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif

namespace {

using Clock = std::chrono::steady_clock;

double NsPerOp(Clock::duration total, std::size_t iterations) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(total).count() /
           static_cast<double>(iterations);
}

volatile std::int64_t g_sink = 0;

// 计时窗口内堆分配清零 / 读取
void AllocReset() { g_allocs.store(0, std::memory_order_relaxed); }
long long AllocCount() { return g_allocs.load(std::memory_order_relaxed); }

}  // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 1000000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--iterations") == 0 && (i + 1) < argc) {
            iterations = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
        }
    }
    if (iterations == 0) { iterations = 1000000; }

    const std::string src = "gateway-1";
    const std::string trc = "bench-trace-0001";
    const std::string pay(128, 'x');       // 小消息（<256B，§22 口径）
    const std::string tx  = "txn-bench";
    const std::string idm = "idem-bench";

    EnvelopeView in;
    in.message_id     = 12345;
    in.message_type   = EnvelopeMessageType::Command;
    in.version        = 1;
    in.source         = src;
    in.timestamp_ms   = 1725000000000LL;
    in.trace_id       = trc;
    in.request_id     = 999;
    in.payload        = pay;
    in.transaction_id  = tx;
    in.idempotency_key = idm;

    const mmo::protocol::ProtobufCodec pb;
    const mmo::protocol::FlatbufCodec fb;

    // ---- 1) Protobuf encode ----
    double pb_encode_ns = 0.0;
    std::string pb_wire;
    {
        AllocReset();
        const auto start = Clock::now();
        for (std::size_t i = 0; i < iterations; ++i) {
            auto r = pb.Encode(in);
            pb_wire.assign(reinterpret_cast<const char*>(r.Value().data()),
                           r.Value().size());
            g_sink = static_cast<std::int64_t>(pb_wire.size()) +
                     static_cast<std::int64_t>(i & 0xFF);
        }
        pb_encode_ns = NsPerOp(Clock::now() - start, iterations);
    }

    // ---- 2) Protobuf decode ----
    double pb_decode_ns = 0.0;
    long long pb_alloc_total = 0;
    {
        AllocReset();
        const auto start = Clock::now();
        for (std::size_t i = 0; i < iterations; ++i) {
            auto r = pb.Decode(pb_wire);
            g_sink = static_cast<std::int64_t>(r.Value().view().payload.size()) +
                     static_cast<std::int64_t>(i & 0xFF);
        }
        const auto elapsed = Clock::now() - start;
        pb_decode_ns = NsPerOp(elapsed, iterations);
        pb_alloc_total = AllocCount();
    }

    // ---- 3) FlatBuffers encode ----
    double fbs_encode_ns = 0.0;
    std::string fbs_wire;
    {
        AllocReset();
        const auto start = Clock::now();
        for (std::size_t i = 0; i < iterations; ++i) {
            auto r = fb.Encode(in);
            fbs_wire.assign(reinterpret_cast<const char*>(r.Value().data()),
                            r.Value().size());
            g_sink = static_cast<std::int64_t>(fbs_wire.size()) +
                     static_cast<std::int64_t>(i & 0xFF);
        }
        fbs_encode_ns = NsPerOp(Clock::now() - start, iterations);
    }

    // ---- 4) FlatBuffers decode（零拷贝：窗口内分配必须为 0） ----
    double fbs_decode_ns = 0.0;
    long long fbs_decode_allocs = -1;
    {
        AllocReset();
        const auto start = Clock::now();
        for (std::size_t i = 0; i < iterations; ++i) {
            auto r = fb.Decode(fbs_wire);
            g_sink = reinterpret_cast<std::int64_t>(r.Value().view().payload.data()) +
                     static_cast<std::int64_t>(i & 0xFF);
        }
        fbs_decode_ns = NsPerOp(Clock::now() - start, iterations);
        fbs_decode_allocs = AllocCount();
    }

    const double alloc_per_op =
        static_cast<double>(pb_alloc_total) / static_cast<double>(iterations);

    char buf[1024];
    const int written = std::snprintf(
        buf, sizeof(buf),
        "pb_encode_ns=%.1f\n"
        "pb_decode_ns=%.1f\n"
        "fbs_encode_ns=%.1f\n"
        "fbs_decode_ns=%.1f\n"
        "alloc_per_op=%.3f\n"
        "fbs_decode_allocs=%lld\n"
        "iterations=%zu\n",
        pb_encode_ns, pb_decode_ns, fbs_encode_ns, fbs_decode_ns,
        alloc_per_op, fbs_decode_allocs, iterations);
    if (written > 0) {
        tprint::Write(buf, static_cast<std::size_t>(written), stdout);
    }

    std::FILE* file = std::fopen("bench/protocol.txt", "w");
    if (file != nullptr) {
        std::fwrite(buf, 1, static_cast<std::size_t>(written), file);
        std::fclose(file);
    } else {
        tprint::Error("WARN: cannot write bench/protocol.txt\n");
    }
    return 0;
}
