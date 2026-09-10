// server/dataservice/tests/data_service_test.cpp
//
// TASK-026 · DataService Interface 测试（§16 单元 / §17 集成 / §19 Failure / §20 验收）。
//
// 覆盖：cache-aside 命中/未命中、版本冲突不覆盖（Store 直测 + Service 经 FakeDataStore）、
// 批量部分失败逐条、TTL 过期、LRU 淘汰、Flush 队列 + 背压 BUSY、存储不可用透传、
// 指标正确、前缀失效、IRepository 编译、1000 次混合 Load/Save 集成场景。
//
// 输出经 test_print.h（禁止裸 cout/printf）。

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "test_print.h"

#include "mmo/core/error/error_code.h"
#include "mmo/data/data_service.h"
#include "mmo/data/fake_data_store.h"
#include "mmo/data/in_memory_cache.h"
#include "mmo/data/in_memory_store.h"
#include "mmo/data/irepository.h"
#include "mmo/data/record.h"

namespace {

using namespace mmo::data;
namespace core = mmo::core;
using core::test::ErrorFmt;

int g_fail = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ErrorFmt("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);  \
            ++g_fail;                                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

Record MakeRec(const DataKey& k, std::uint32_t ver, const std::string& payload = "pl") {
    Record r;
    r.key = k;
    r.version = ver;
    r.payload = payload;
    return r;
}

void test_cache_aside_hit_miss() {
    InMemoryCache cache;
    InMemoryStore store;
    DataService ds(cache, store);
    CHECK(ds.Save(MakeRec("character:1", 1)).HasValue());
    auto loaded = ds.Load("character:1");
    CHECK(loaded.HasValue());
    CHECK(loaded.Value().has_value());
    CHECK(loaded.Value().value().version == 1);
    CHECK(ds.Stats().hit_rate > 0.99);            // 全命中（此时仅有一次命中 Load）
    auto miss = ds.Load("character:999");
    CHECK(miss.HasValue());
    CHECK(!miss.Value().has_value());             // 未命中
    CHECK(ds.Stats().hit_rate > 0.33 && ds.Stats().hit_rate < 0.67);  // 1/2
}

void test_version_conflict_store() {
    InMemoryStore store;
    CHECK(store.Save(MakeRec("k", 1), VersionCheck{0, true}).HasValue());
    auto conflict = store.Save(MakeRec("k", 2), VersionCheck{0, true});
    CHECK(!conflict.HasValue());
    CHECK(conflict.Err().Code() == core::ErrorCode::VERSION_CONFLICT);
    CHECK(store.conflict_count() == 1);
    auto cur = store.Load("k");
    CHECK(cur.HasValue() && cur.Value().has_value());
    CHECK(cur.Value().value().version == 1);     // 未被静默覆盖
}

void test_version_conflict_via_service() {
    // DataService::Save 是 write-behind：先写缓存并入脏队列，立即返回 Ok，
    // 不会当场冲突。版本冲突在 Flush 落盘时由权威存储裁决并统计到 Stats。
    InMemoryCache cache;
    FakeDataStore fake;
    fake.set_force_conflict(true);   // 模拟外部并发写入者持续冲突
    DataService ds(cache, fake);
    CHECK(ds.Save(MakeRec("k", 1)).HasValue());  // 写缓存 + 入脏队列
    CHECK(ds.Flush().HasValue());                // 落盘触发冲突，但 Flush 不崩溃
    CHECK(ds.Stats().conflict_count >= 1);       // 冲突被统计（禁止静默丢弃）
    CHECK(ds.pending_writes() == 1);             // 冲突记录重新入队，未丢失
}

void test_batch_partial() {
    InMemoryStore store;
    CHECK(store.Save(MakeRec("A", 1), VersionCheck{0, true}).HasValue());
    std::vector<Record> recs = {MakeRec("A", 5), MakeRec("B", 0), MakeRec("C", 0)};
    auto r = store.BatchSave(recs);
    CHECK(r.HasValue());
    const auto& out = r.Value();
    CHECK(out.size() == 3);
    std::size_t ok = 0, cf = 0;
    for (const auto& o : out) {
        if (o.ok) ++ok;
        else if (o.code == static_cast<std::uint32_t>(core::ErrorCode::VERSION_CONFLICT)) ++cf;
    }
    CHECK(ok == 2);
    CHECK(cf == 1);
    CHECK(store.Load("B").Value().has_value());
    CHECK(store.Load("C").Value().has_value());
    CHECK(store.Load("A").Value().has_value());   // A 未覆盖，仍保留旧版本
    CHECK(store.Load("A").Value().value().version == 1);
}

void test_ttl_expiry() {
    InMemoryCache cache(1024);
    CHECK(cache.Put(MakeRec("k", 1), core::DurationMs{1}).HasValue());
    auto hit = cache.Get("k");
    CHECK(hit.HasValue() && hit.Value().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    auto expired = cache.Get("k");
    CHECK(expired.HasValue() && !expired.Value().has_value());
    CHECK(cache.misses() >= 1);  // 过期后再次 Get 计为 1 次 miss
}

void test_lru_eviction() {
    InMemoryCache cache(2);
    CHECK(cache.Put(MakeRec("a", 1)).HasValue());
    CHECK(cache.Put(MakeRec("b", 1)).HasValue());
    CHECK(cache.Put(MakeRec("c", 1)).HasValue());   // 超容量，淘汰最久未用
    CHECK(cache.size() == 2);
    CHECK(cache.evictions() >= 1);
    auto a = cache.Get("a");
    CHECK(a.HasValue() && !a.Value().has_value());  // a 已淘汰
    auto b = cache.Get("b");
    CHECK(b.HasValue() && b.Value().has_value());
}

void test_flush_and_backpressure() {
    InMemoryCache cache;
    InMemoryStore store;
    DataService ds(cache, store, /*max_pending=*/2);
    CHECK(ds.Save(MakeRec("x1", 1)).HasValue());
    CHECK(ds.Save(MakeRec("x2", 1)).HasValue());
    auto busy = ds.Save(MakeRec("x3", 1));           // 队列满
    CHECK(!busy.HasValue());
    CHECK(busy.Err().Code() == core::ErrorCode::BUSY);
    CHECK(ds.pending_writes() == 2);
    CHECK(ds.Flush().HasValue());
    CHECK(ds.pending_writes() == 0);
    CHECK(store.size() == 2);
    CHECK(store.Load("x1").Value().has_value());
    CHECK(store.Load("x2").Value().has_value());
    CHECK(ds.Flush().HasValue());                    // 空 flush 也安全
}

void test_store_unavailable() {
    InMemoryCache cache;
    FakeDataStore fake;
    fake.set_fail_loads(true);
    DataService ds(cache, fake);
    auto r = ds.Load("any");
    CHECK(!r.HasValue());
    CHECK(r.Err().Code() == core::ErrorCode::INTERNAL_ERROR);  // 故障透传，非崩溃
}

void test_stats() {
    InMemoryCache cache;
    InMemoryStore store;
    DataService ds(cache, store);
    ds.Save(MakeRec("k", 1));
    ds.Load("k");
    ds.Load("k");
    ds.Load("missing");
    auto st = ds.Stats();
    CHECK(st.hit_rate > 0.5);                        // 2 hits / 3
    ds.Flush();
    CHECK(ds.Stats().flush_count >= 1);
}

void test_invalidate() {
    InMemoryCache cache;
    CHECK(cache.Put(MakeRec("inv:a", 1)).HasValue());
    CHECK(cache.Put(MakeRec("inv:b", 1)).HasValue());
    CHECK(cache.InvalidatePrefix("inv:").HasValue());
    CHECK(cache.size() == 0);
}

void test_irepository_compiles() {
    // IRepository<T> 是模板接口；仅验证可实例化（语义由具体仓储实现保证，见 §27.4）。
    IRepository<int>* p = nullptr;
    (void)p;
    CHECK(true);
}

void test_mixed_1000() {
    InMemoryCache cache;
    InMemoryStore store;
    DataService ds(cache, store);
    for (int i = 0; i < 1000; ++i) {
        const DataKey k = "ent:" + std::to_string(i);
        if (i % 2 == 0) {
            CHECK(ds.Save(MakeRec(k, 1)).HasValue());
        } else {
            auto r = ds.Load(k);
            CHECK(r.HasValue());
        }
    }
    CHECK(ds.Flush().HasValue());
    std::size_t landed = 0;
    for (int i = 0; i < 1000; i += 2) {
        if (store.Load("ent:" + std::to_string(i)).Value().has_value()) ++landed;
    }
    CHECK(landed == 500);                            // 偶数键全部落盘
}

}  // namespace

int main() {
    test_cache_aside_hit_miss();
    test_version_conflict_store();
    test_version_conflict_via_service();
    test_batch_partial();
    test_ttl_expiry();
    test_lru_eviction();
    test_flush_and_backpressure();
    test_store_unavailable();
    test_stats();
    test_invalidate();
    test_irepository_compiles();
    test_mixed_1000();

    if (g_fail == 0) {
        ErrorFmt("DataService.Suite: ALL PASS\n");
        return 0;
    }
    ErrorFmt("DataService.Suite: %d FAIL\n", g_fail);
    return g_fail;
}
