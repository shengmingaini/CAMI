// server/dataservice/benchmark/data_bench.cpp
//
// TASK-026 · DataService Benchmark（§18）。
//
// 输出机器可读的 bench/data.txt（key=value），供验收脚本 assert_metric 解析：
//   load_ns / save_ns / batch_load_ns_per_1k / cache_hit_ns / flush_ns_per_1k
// 验收阈值：load_ns <= 500、cache_hit_ns <= 300。
//
// 输出经 test_print.h（禁止裸 cout/printf）；文件用 C FILE* 写（禁止 std::ifstream）。

#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/data/data_service.h"
#include "mmo/data/in_memory_cache.h"
#include "mmo/data/in_memory_store.h"
#include "mmo/data/record.h"

namespace {

using namespace mmo::data;
namespace core = mmo::core;
using core::test::LineFmt;

double now_ns() {
    return std::chrono::duration<double, std::nano>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// 测单条操作平均耗时（ns），扣除 steady_clock::now() 计时开销。
template <typename F>
double avg_ns(std::uint64_t ops, F fn) {
    double t0 = now_ns();
    for (std::uint64_t i = 0; i < ops; ++i) {
        volatile double x = now_ns();
        (void)x;
    }
    double t1 = now_ns();
    const double overhead = (t1 - t0) / static_cast<double>(ops);

    t0 = now_ns();
    for (std::uint64_t i = 0; i < ops; ++i) fn(i);
    t1 = now_ns();
    double per = (t1 - t0) / static_cast<double>(ops) - overhead;
    return per > 0.0 ? per : 0.0;
}

Record MakeRec(const DataKey& k, std::uint32_t ver) {
    Record r;
    r.key = k;
    r.version = ver;
    r.payload = "payload-bytes";
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t ops = 100000;
    std::string out_path = "bench/data.txt";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ops" && i + 1 < argc) ops = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
    }

    constexpr std::size_t N = 1000;

    // 预填充权威存储与缓存（缓存用无 TTL，避免 Get 内部 now() 抖动）。
    InMemoryStore store;
    InMemoryCache cache(N);
    for (std::size_t i = 0; i < N; ++i) {
        (void)store.Save(MakeRec("k:" + std::to_string(i), 1), VersionCheck{0, false});
        (void)cache.Put(MakeRec("k:" + std::to_string(i), 1));
    }

    const double load_ns = avg_ns(ops, [&](std::uint64_t i) {
        auto r = store.Load("k:" + std::to_string(i % N));
        (void)r;
    });

    const double save_ns = avg_ns(ops, [&](std::uint64_t i) {
        // 每轮写入新键（无版本冲突），代表权威写入成本。
        auto r = store.Save(MakeRec("s:" + std::to_string(i), 1), VersionCheck{0, false});
        (void)r;
    });

    const double cache_hit_ns = avg_ns(ops, [&](std::uint64_t i) {
        auto r = cache.Get("k:" + std::to_string(i % N));
        (void)r;
    });

    // 批量加载 1000 键 / 次。
    std::vector<DataKey> keys;
    keys.reserve(N);
    for (std::size_t i = 0; i < N; ++i) keys.push_back("k:" + std::to_string(i));
    const std::uint64_t batch_iters = (ops / N) > 0 ? (ops / N) : 1;
    double t0 = now_ns();
    for (std::uint64_t it = 0; it < batch_iters; ++it) {
        auto r = store.BatchLoad(keys);
        (void)r;
    }
    double t1 = now_ns();
    const double batch_load_ns_per_1k = (t1 - t0) / static_cast<double>(batch_iters);

    // Flush 1000 脏记录 / 次（每次重填脏队列）。
    DataService ds(cache, store, /*max_pending=*/N + 1);
    const std::uint64_t flush_iters = batch_iters;
    t0 = now_ns();
    for (std::uint64_t it = 0; it < flush_iters; ++it) {
        for (std::size_t i = 0; i < N; ++i) (void)ds.Save(MakeRec("f:" + std::to_string(i), 1));
        (void)ds.Flush();
    }
    t1 = now_ns();
    const double flush_ns_per_1k = (t1 - t0) / static_cast<double>(flush_iters);

    // 写文件（C FILE*，避免 std::ifstream）。
    std::FILE* fp = std::fopen(out_path.c_str(), "w");
    if (fp) {
        std::fprintf(fp,
                     "load_ns=%.3f\n"
                     "save_ns=%.3f\n"
                     "batch_load_ns_per_1k=%.3f\n"
                     "cache_hit_ns=%.3f\n"
                     "flush_ns_per_1k=%.3f\n",
                     load_ns, save_ns, batch_load_ns_per_1k, cache_hit_ns, flush_ns_per_1k);
        std::fclose(fp);
    } else {
        LineFmt("WARN: cannot open %s for write\n", out_path.c_str());
    }

    LineFmt("load_ns=%.3f\n", load_ns);
    LineFmt("save_ns=%.3f\n", save_ns);
    LineFmt("batch_load_ns_per_1k=%.3f\n", batch_load_ns_per_1k);
    LineFmt("cache_hit_ns=%.3f\n", cache_hit_ns);
    LineFmt("flush_ns_per_1k=%.3f\n", flush_ns_per_1k);
    return 0;
}
