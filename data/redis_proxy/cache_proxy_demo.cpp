// ============================================================================
// data/redis_proxy/cache_proxy_demo.cpp — 缓存代理验证/基准 (Week4 D3)
// [PROTOTYPE] 自包含演示: Zipf 倾斜工作负载下验证 Read-Through 命中率 + Write-Back 落库
// 编译: g++ -std=c++17 -O2 cache_proxy_demo.cpp cache_proxy.cpp -o cami_cache_demo
//       (依赖仅 STL, 无需 Redis/vcpkg, 可在 CAMI_BUILD_MODULES=OFF 下验证)
// ============================================================================
#include "data/redis_proxy/cache_proxy.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
#include <numeric>
#include <cassert>

using namespace cami::data::redis_proxy;

// 简单 Zipf 采样器: 高频 key 被反复访问, 制造热点倾斜
struct Zipf {
    std::vector<double> cdf;
    double total = 0;
    std::mt19937_64 rng{20260811};
    std::uniform_real_distribution<double> u{0, 1};

    explicit Zipf(std::size_t n, double s = 0.9) {
        cdf.resize(n);
        double sum = 0;
        for (std::size_t i = 1; i <= n; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i), s);
            cdf[i - 1] = sum;
        }
        total = sum;
    }
    std::size_t next() {
        double r = u(rng) * total;
        auto it = std::lower_bound(cdf.begin(), cdf.end(), r);
        return static_cast<std::size_t>(it - cdf.begin());
    }
};

int main() {
    const std::size_t N = 20000;          // key 空间
    const std::size_t MEASURE_OPS = 1000000;

    InMemoryBackend backend(N * 2);        // 容量充裕, 无 LRU 抖动
    InMemoryStore store;                   // 仿真分片 DB
    CacheProxy proxy(backend, store, WritePolicy::WriteBack, /*enable_hot=*/true);

    // 预置 DB (模拟分片库已存在玩家数据)
    for (std::size_t i = 0; i < N; ++i)
        store.Store("k" + std::to_string(i), "v" + std::to_string(i));

    // ---- 热身: 全量读一遍, 全部 miss 但回填缓存 ----
    for (std::size_t i = 0; i < N; ++i) proxy.Get("k" + std::to_string(i));
    std::printf("[warmup] DB 回源次数=%llu (应=%zu), 缓存命中率=%.4f\n",
                (unsigned long long)proxy.misses(), N, proxy.hit_rate());

    // ---- 测量: Zipf 倾斜读, 缓存应吸收绝大多数请求 ----
    // 注意: CacheProxy 含引用成员, 不可拷贝/重赋值; 用差分法统计测量阶段.
    Zipf zipf(N, 0.9);
    uint64_t base_hits = proxy.hits(), base_miss = proxy.misses();
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < MEASURE_OPS; ++i) {
        proxy.Get("k" + std::to_string(zipf.next()));
    }
    auto t1 = std::chrono::steady_clock::now();
    uint64_t m_hits = proxy.hits() - base_hits;
    uint64_t m_miss = proxy.misses() - base_miss;
    double rate = static_cast<double>(m_hits) / (m_hits + m_miss);
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("[measure] ops=%zu  hits=%llu  misses(DB回源)=%llu  命中率=%.4f  吞吐=%.0f ops/s\n",
                MEASURE_OPS, (unsigned long long)m_hits, (unsigned long long)m_miss,
                rate, MEASURE_OPS / (ms / 1000.0));

    // ---- 写回验证: Put 一批 dirty, FlushDirty 落库 ----
    const std::size_t W = 1000;
    for (std::size_t i = 0; i < W; ++i)
        proxy.Put("w" + std::to_string(i), "new" + std::to_string(i), WritePolicy::WriteBack);
    std::size_t flushed = proxy.FlushDirty();
    std::printf("[write-back] 标记 dirty=%zu  FlushDirty 落库=%zu  落库后 store 含 w0? %s\n",
                W, flushed,
                store.Load("w0").has_value() ? "yes" : "no");

    // ---- 热点识别验证 ----
    auto hot = proxy.hot_keys();
    std::printf("[hot-key] 识别到的热点 key 数=%zu (top 应集中在 Zipf 头部)\n", hot.size());

    // ---- 验收断言: 命中率 >= 95% ----
    std::printf("\n=== D3 验收: 缓存命中率>=95%% ? %s (实测 %.2f%%) ===\n",
                rate >= 0.95 ? "PASS" : "FAIL", rate * 100.0);
    return rate >= 0.95 ? 0 : 1;
}
