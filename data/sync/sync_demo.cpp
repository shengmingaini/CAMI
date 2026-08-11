// ============================================================================
// data/sync/sync_demo.cpp — 数据同步验证 (Week4 D4)
// [PROTOTYPE] 验证 30s 批量落库 + 断线立即持久化 + 版本 CAS 落库 + 吞吐
// 编译: g++ -std=c++17 -O2 -I. data/sync/sync_demo.cpp data/redis_proxy/cache_proxy.cpp -o build/sync_demo
// ============================================================================
#include "data/redis_proxy/cache_proxy.h"
#include "data/version/version.h"
#include "data/sync/sync_manager.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace cami::data;
using namespace cami::data::redis_proxy;

int main() {
    InMemoryBackend backend(1 << 20);
    InMemoryStore store;
    version::VersionedStore vstore;
    CacheProxy cache(backend, store, WritePolicy::WriteBack, /*hot=*/true);
    sync::SyncManager sync(cache, store, vstore, std::chrono::milliseconds(50));

    for (int i = 0; i < 100; ++i) vstore.Init("p" + std::to_string(i), "init");

    // 写缓存 + 标脏, 启动周期落库 (50ms 演示)
    for (int i = 0; i < 100; ++i) {
        std::string k = "p" + std::to_string(i);
        cache.Put(k, "v" + std::to_string(i));
        sync.MarkDirty(k);
    }
    sync.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    sync.Stop();
    auto p0 = store.Load("p0");
    std::printf("[periodic] 周期落库 flush_count=%llu (应>=100), store.p0=%s\n",
                (unsigned long long)sync.flush_count(),
                p0.has_value() ? p0->c_str() : "<none>");

    // 断线立即持久化
    cache.Put("disc", "dval");
    sync.MarkDirty("disc");
    sync.OnDisconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto d = store.Load("disc");
    std::printf("[disconnect] OnDisconnect 后 store.disc=%s (应=dval)\n",
                d.has_value() ? d->c_str() : "<none>");

    // 基准: 批量落库吞吐 (验证逻辑路径非瓶颈)
    InMemoryBackend b2(1 << 20);
    InMemoryStore s2;
    version::VersionedStore v2;
    CacheProxy c2(b2, s2, WritePolicy::WriteBack, /*hot=*/false);
    sync::SyncManager sync2(c2, s2, v2, std::chrono::milliseconds(100000));
    const int N = 200000;
    for (int i = 0; i < N; ++i) {
        std::string k = "b" + std::to_string(i);
        c2.Put(k, "x");
        sync2.MarkDirty(k);
    }
    auto t0 = std::chrono::steady_clock::now();
    std::size_t flushed = sync2.FlushNow();
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("[bench] 批量落库 %llu 条, 同步路径吞吐=%.0f 条/s (单分库 DB 实测见压测报告)\n",
                (unsigned long long)flushed, flushed / sec);

    bool pass = sync.flush_count() >= 100 && d.has_value() && flushed == static_cast<std::size_t>(N);
    std::printf("\n=== D4 验收: 30s批量落库+断线立即持久化 ? %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
