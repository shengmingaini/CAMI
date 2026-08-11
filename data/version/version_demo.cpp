// ============================================================================
// data/version/version_demo.cpp — 版本校验验证 (Week4 D5)
// [PROTOTYPE] 验证乐观锁 CAS: 并发写冲突 100% 被版本号拦截, 无丢失更新
// 编译: g++ -std=c++17 -O2 -I. data/version/version_demo.cpp -o build/version_demo
// ============================================================================
#include "data/version/version.h"
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace cami::data::version;

int main() {
    VersionedStore store;
    store.Init("p1", "v0");

    // 测试1: 8 线程并发 Cas(expected=0) -> 仅 1 个成功 (其余被版本拦截)
    std::atomic<int> wins{0};
    const int T = 8;
    std::vector<std::thread> ts;
    for (int i = 0; i < T; ++i)
        ts.emplace_back([&] { if (store.Cas("p1", "vx", 0)) wins.fetch_add(1); });
    for (auto& t : ts) t.join();
    std::printf("[conflict] 8 线程并发 Cas(expected=0): 成功=%d (应=1)\n", wins.load());

    // 测试2: 乐观自增 (读版本->Cas), 冲突重试 -> 无丢失更新
    store.Init("cnt", "0");
    std::atomic<uint64_t> done{0};
    auto worker = [&](int rounds) {
        for (int i = 0; i < rounds; ++i) {
            for (;;) {
                auto cur = store.Load("cnt");
                uint64_t v = std::stoull(cur->value);
                std::string nv = std::to_string(v + 1);
                if (store.Cas("cnt", nv, cur->version)) { done.fetch_add(1); break; }
                // 版本冲突 -> 重试
            }
        }
    };
    std::vector<std::thread> ts2;
    int per = 2000;
    for (int i = 0; i < T; ++i) ts2.emplace_back(worker, per);
    for (auto& t : ts2) t.join();
    auto final = store.Load("cnt");
    uint64_t expect = static_cast<uint64_t>(T) * per;
    std::printf("[optimistic] 并发自增: 成功=%llu 最终值=%s 期望=%llu\n",
                (unsigned long long)done.load(), final->value.c_str(), (unsigned long long)expect);

    bool pass = (wins.load() == 1) && (std::stoull(final->value) == expect);
    std::printf("\n=== D5 验收: 并发写冲突 100%% 被版本号拦截 ? %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
