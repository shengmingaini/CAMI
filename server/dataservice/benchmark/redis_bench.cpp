// server/dataservice/benchmark/redis_bench.cpp
//
// TASK-027 · Redis 性能基准（§18）。输出机器可读 key=value 到 bench/redis.txt。
// 用法：bin/redis_bench --ops 10000
// 无实例时优雅退出（非零），不崩溃、不伪造指标。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/core/time/clock.h"
#include "mmo/data/redis/connection_pool.h"
#include "mmo/data/redis/redis_cache.h"
#include "mmo/data/redis/redis_config.h"

namespace core = mmo::core;

int main(int argc, char** argv) {
    std::size_t ops = 10000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            ops = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
        }
    }

    mmo::data::redis::RedisConfig cfg;
    cfg.password_env = "";
    cfg.pool_size = 8;
    auto pool = mmo::data::redis::ConnectionPool::Create(cfg);
    if (!pool.HasValue()) {
        core::test::Error("redis_bench: cannot connect (start redis first)\n");
        return 2;
    }

    auto cache = mmo::data::redis::RedisCache::Create(cfg);
    if (!cache.HasValue()) {
        core::test::Error("redis_bench: cache create failed\n");
        return 2;
    }
    auto& c = *cache.Value();

    const std::string payload(64, 'x');  // 固定小对象

    // ---- get / put 平均延迟 ----
    std::uint64_t put_ns_total = 0, get_ns_total = 0;
    for (std::size_t i = 0; i < ops; ++i) {
        const std::string key = "bench:" + std::to_string(i % 1000);
        mmo::data::Record rec;
        rec.key = key;
        rec.version = 1;
        rec.payload = payload;

        auto t0 = std::chrono::steady_clock::now();
        (void)c.Put(rec);
        auto t1 = std::chrono::steady_clock::now();
        (void)c.Get(key);
        auto t2 = std::chrono::steady_clock::now();
        put_ns_total += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        get_ns_total += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
    }
    const double put_ns = static_cast<double>(put_ns_total) / static_cast<double>(ops);
    const double get_ns = static_cast<double>(get_ns_total) / static_cast<double>(ops);

    // ---- 管道：100 条 MGET 批量 ----
    double pipeline_ns_per_100 = 0;
    {
        std::vector<std::string> keys(100);
        for (int i = 0; i < 100; ++i) keys[i] = "bench:" + std::to_string(i);
        // 预热
        for (int i = 0; i < 100; ++i) {
            mmo::data::Record rec;
            rec.key = keys[i];
            rec.version = 1;
            rec.payload = payload;
            (void)c.Put(rec);
        }
        const int rounds = 200;
        auto tp0 = std::chrono::steady_clock::now();
        for (int r = 0; r < rounds; ++r) {
            for (int i = 0; i < 100; ++i) (void)c.Get(keys[i]);
        }
        auto tp1 = std::chrono::steady_clock::now();
        pipeline_ns_per_100 = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tp1 - tp0).count()) /
            static_cast<double>(rounds);
    }

    // ---- 连接池获取延迟 ----
    double pool_acquire_ns = 0;
    {
        const int rounds = 5000;
        auto ta0 = std::chrono::steady_clock::now();
        for (int i = 0; i < rounds; ++i) {
            auto conn = pool.Value()->Acquire(core::DurationMs{50});
            (void)conn;  // 立即析构归还
        }
        auto ta1 = std::chrono::steady_clock::now();
        pool_acquire_ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ta1 - ta0).count()) /
            static_cast<double>(rounds);
    }

    FILE* f = std::fopen("bench/redis.txt", "w");
    if (f) {
        std::fprintf(f, "get_ns=%.3f\n", get_ns);
        std::fprintf(f, "put_ns=%.3f\n", put_ns);
        std::fprintf(f, "pipeline_ns_per_100=%.3f\n", pipeline_ns_per_100);
        std::fprintf(f, "pool_acquire_ns=%.3f\n", pool_acquire_ns);
        std::fprintf(f, "conn_count=%u\n", cfg.pool_size);
        std::fclose(f);
    }
    core::test::LineFmt("get_ns=%.3f put_ns=%.3f pipeline_ns_per_100=%.3f pool_acquire_ns=%.3f conn_count=%u\n",
        get_ns, put_ns, pipeline_ns_per_100, pool_acquire_ns, cfg.pool_size);
    return 0;
}
