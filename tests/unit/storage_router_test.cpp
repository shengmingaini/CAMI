// ============================================================================
// tests/unit/storage_router_test.cpp — 自主存储治理器单测 (OFF 可编译, 纯 STL)
// 用 InMemoryStore 仿真双端 + ControllableStore/FailingStore 验证熔断与降级。
// 不依赖 RocksDB。
// ============================================================================
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <unordered_map>

#include "data/redis_proxy/cache_proxy.h"
#include "data/rocksdb_proxy/storage_router.h"

using namespace cami::data;
using namespace cami::data::rocksdb_proxy;

namespace {

// 可切换故障的候选后端: 正常时等同内存存储, fail=true 时每次操作抛异常。
class ControllableStore : public redis_proxy::BackingStore {
public:
    bool fail = false;

    std::optional<std::string> Load(std::string_view key) override {
        if (fail) throw std::runtime_error("boom");
        auto it = db_.find(std::string(key));
        return it == db_.end() ? std::nullopt : std::optional<std::string>{it->second.first};
    }
    void Store(std::string_view key, std::string_view value) override {
        if (fail) throw std::runtime_error("boom");
        std::string k(key);
        auto it = db_.find(k);
        if (it == db_.end()) db_.emplace(k, Row{std::string(value), 1});
        else { it->second.first = std::string(value); it->second.second += 1; }
    }
    std::optional<redis_proxy::StoreRow> LoadWithVersion(std::string_view key) override {
        if (fail) throw std::runtime_error("boom");
        auto it = db_.find(std::string(key));
        return it == db_.end()
                   ? std::nullopt
                   : std::optional<redis_proxy::StoreRow>{
                         redis_proxy::StoreRow{it->second.first, it->second.second}};
    }
    bool CasStore(std::string_view key, std::string_view value, uint64_t expected) override {
        if (fail) throw std::runtime_error("boom");
        std::string k(key);
        auto it = db_.find(k);
        if (it == db_.end()) {
            if (expected != 0) return false;
            db_.emplace(k, Row{std::string(value), 1});
            return true;
        }
        if (it->second.second != expected) return false;
        it->second.first = std::string(value);
        it->second.second += 1;
        return true;
    }
    void Delete(std::string_view key) override {
        if (fail) throw std::runtime_error("boom");
        db_.erase(std::string(key));
    }

private:
    struct Row { std::string first; uint64_t second = 0; };
    std::unordered_map<std::string, Row> db_;
};

// 总是抛异常的候选后端, 用于触发熔断。
class FailingStore : public redis_proxy::BackingStore {
public:
    std::optional<std::string> Load(std::string_view) override { throw std::runtime_error("boom"); }
    void Store(std::string_view, std::string_view) override { throw std::runtime_error("boom"); }
    std::optional<redis_proxy::StoreRow> LoadWithVersion(std::string_view) override {
        throw std::runtime_error("boom");
    }
    bool CasStore(std::string_view, std::string_view, uint64_t) override {
        throw std::runtime_error("boom");
    }
    void Delete(std::string_view) override { throw std::runtime_error("boom"); }
};

TEST(StorageRouterTest, ShadowServesBaselineAndPromotes) {
    // 双端预置相同状态 (候选与基线一致, 不应有偏差)。
    ControllableStore primary;
    redis_proxy::InMemoryStore fallback;
    primary.Store("k1", "v1");
    fallback.Store("k1", "v1");

    StorageRouter router(primary, fallback, /*promote_after_ops=*/10, /*max_mismatch_rate=*/0.0);
    EXPECT_EQ(router.mode(), RouterMode::Shadow);

    for (int i = 0; i < 10; ++i) {
        auto v = router.Load("k1");
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, "v1");  // 影子模式读走基线
    }

    router.maybe_promote();
    EXPECT_EQ(router.mode(), RouterMode::ActivePrimary);
    EXPECT_DOUBLE_EQ(router.score().mismatch_rate(), 0.0);

    // 晋升后读应来自候选 (primary 已含 v1)。
    auto v = router.Load("k1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "v1");
}

TEST(StorageRouterTest, ShadowCountsMismatch) {
    ControllableStore primary;
    redis_proxy::InMemoryStore fallback;
    primary.Store("k1", "from_candidate");
    fallback.Store("k1", "from_baseline");  // 候选与基线不一致

    StorageRouter router(primary, fallback, 100, 0.0);
    auto v = router.Load("k1");
    EXPECT_EQ(*v, "from_baseline");  // 影子读基线, 不暴露候选错误值
    EXPECT_GE(router.score().mismatches.load(), 1u);
}

TEST(StorageRouterTest, CircuitBreakerTripsOnPrimaryFailure) {
    FailingStore primary;
    redis_proxy::InMemoryStore fallback;

    StorageRouter router(primary, fallback, 100, 0.0);
    std::string alert_msg;
    router.set_alert_handler([&](const std::string& m) { alert_msg = m; });

    // 候选连续 5 次失败 -> 熔断, 全量回退基线 + 告警。
    for (int i = 0; i < 5; ++i) router.Store("k", "v");

    EXPECT_TRUE(router.breaker().tripped());
    EXPECT_EQ(router.mode(), RouterMode::FallbackOnly);
    EXPECT_FALSE(alert_msg.empty());

    // 熔断后读仍可从基线服务 (不抛异常, 无数据丢失)。
    fallback.Store("kx", "vx");
    auto v = router.Load("kx");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "vx");
}

TEST(StorageRouterTest, CasStoreDualWriteConsistent) {
    ControllableStore primary;
    redis_proxy::InMemoryStore fallback;
    primary.Store("p1", "init");
    fallback.Store("p1", "init");  // 双端 version=1, payload=init

    StorageRouter router(primary, fallback, 100, 0.0);
    bool ok = router.CasStore("p1", "new", /*expected_version=*/1);
    EXPECT_TRUE(ok);
    EXPECT_EQ(router.score().mismatches.load(), 0u);  // 候选与基线裁决一致

    auto vp = primary.Load("p1");
    auto vb = fallback.Load("p1");
    ASSERT_TRUE(vp.has_value());
    ASSERT_TRUE(vb.has_value());
    EXPECT_EQ(*vp, "new");
    EXPECT_EQ(*vb, "new");
}

TEST(StorageRouterTest, ActivePrimaryDegradesToBaselineOnCandidateReadFailure) {
    ControllableStore primary;
    redis_proxy::InMemoryStore fallback;
    primary.Store("kx", "vx");
    fallback.Store("kx", "vx");  // 双端一致, 影子期可正常评估

    // 影子期跑满 50 次 -> 达标晋升主动主。
    StorageRouter router(primary, fallback, /*promote_after_ops=*/50, 0.0);
    for (int i = 0; i < 50; ++i) {
        auto v = router.Load("kx");
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, "vx");
    }
    router.maybe_promote();
    ASSERT_EQ(router.mode(), RouterMode::ActivePrimary);

    // 候选开始读失败 -> 降级基线, 不抛异常, 模式保持 (单点故障不致命)。
    primary.fail = true;
    auto v = router.Load("kx");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "vx");
}

}  // namespace
