// server/dataservice/tests/redis_test.cpp
//
// TASK-027 · Redis 适配器测试（§16 单元 / §17 集成 / §19 Failure / §20 验收）。
//
// 设计：纯逻辑单测（配置解析 / 键名 / 序列化 / 重试判定 / 熔断状态机 / 池探测失败）
//       不依赖真实实例，离线必跑；标注 [redis] 的集成用例在实例不可达时明确 SKIP
//       （打印 SKIP 且不计入失败，禁止伪装通过，§20.7）。
//
// 输出经 test_print.h（禁止裸 cout/printf）。

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "test_print.h"

#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/data/health.h"
#include "mmo/data/record.h"
#include "mmo/data/redis/circuit_breaker.h"
#include "mmo/data/redis/connection_pool.h"
#include "mmo/data/redis/keys.h"
#include "mmo/data/redis/redis_cache.h"
#include "mmo/data/redis/redis_config.h"
#include "mmo/data/redis/retry.h"
#include "mmo/data/redis/serialization.h"

namespace {

using namespace mmo::data;
using namespace mmo::data::redis;
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
    r.updated_at = core::SteadyTime(std::chrono::nanoseconds(123456789LL));
    return r;
}

bool RedisAvailable() {
    RedisConfig cfg;
    cfg.password_env = "";
    cfg.connect_timeout = core::DurationMs{200};
    auto p = ConnectionPool::Create(cfg);
    return p.HasValue();
}

// ---- §16 单元：配置解析（密码从环境变量读，缺失报错）----
void test_resolve_password_env() {
    RedisConfig with_env;
    with_env.password_env = "MMORPG_REDIS_PASSWORD_UNSET_XX";
    auto r = ResolveRedisPassword(with_env);
    CHECK(!r.HasValue());
    CHECK(r.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);

    RedisConfig no_env;
    no_env.password_env = "";
    auto r2 = ResolveRedisPassword(no_env);
    CHECK(r2.HasValue());
    CHECK(r2.Value().empty());  // 无需认证
}

// ---- §8 / §15.7 键空间规范 ----
void test_key_generation() {
    CHECK(SessionKey("abc") == "sess:abc");
    CHECK(PlayerRouteKey("42") == "route:player:42");
    CHECK(SceneRouteKey("7") == "route:scene:7");
    CHECK(CharCacheKey("99") == "cache:char:99");
    CHECK(InvCacheKey("99") == "cache:inv:99");
}

// ---- §15.4 序列化往返 + 损坏容错 ----
void test_serialization_roundtrip() {
    Record rec = MakeRec("character:1", 3, "hello-payload");
    const std::string blob = EncodeRecord(rec);
    auto dec = DecodeRecord(blob);
    CHECK(dec.HasValue());
    CHECK(dec.Value().key == rec.key);
    CHECK(dec.Value().version == rec.version);
    CHECK(dec.Value().payload == rec.payload);
    CHECK(dec.Value().updated_at == rec.updated_at);  // 单调时刻精确往返
}

void test_serialization_corrupt() {
    CHECK(!DecodeRecord("not-a-record").HasValue());     // 魔数错误
    CHECK(!DecodeRecord(std::string("MMO1", 4)).HasValue());  // 过短
    std::string truncated = EncodeRecord(MakeRec("k", 1, "x"));
    truncated.resize(truncated.size() - 2);  // 截断
    CHECK(!DecodeRecord(truncated).HasValue());
}

// ---- §15.5 重试判定 ----
void test_retry_classify() {
    CHECK(IsRetryableRedisError(4 /*TIMEOUT*/, "timeout") == true);
    CHECK(IsRetryableRedisError(3 /*IO*/, "Connection refused") == true);
    CHECK(IsRetryableRedisError(0, "WRONGTYPE") == false);   // 业务错误不重试
    CHECK(IsRetryableRedisError(0, "protocol error") == false);
}

// ---- §15.8 熔断状态机 ----
void test_circuit_breaker_fsm() {
    CircuitBreaker b(/*threshold=*/3, core::DurationMs{1});
    CHECK(b.AllowRequest() == true);
    CHECK(b.health() == HealthStatus::Healthy);
    b.RecordFailure();
    b.RecordFailure();
    CHECK(b.state() == CircuitBreaker::State::Closed);  // 未达阈值
    b.RecordFailure();                                   // 第 3 次 -> Open
    CHECK(b.state() == CircuitBreaker::State::Open);
    CHECK(b.AllowRequest() == false);
    CHECK(b.health() == HealthStatus::Unavailable);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));  // 等冷却
    CHECK(b.AllowRequest() == true);                    // 冷却到期 -> HalfOpen
    b.RecordSuccess();
    CHECK(b.state() == CircuitBreaker::State::Closed);  // 探活成功 -> 恢复
    CHECK(b.health() == HealthStatus::Healthy);
}

// ---- §19 Failure：Redis 不可达（连必死端口 6390），Create 返回明确错误（不崩溃、不挂死）----
// 注意：不能依赖「真实 Redis 宕机」做前提——实例在线时此用例会误失败。
//       改为连一个确定不监听的端口，使失败路径测试与实例在线状态解耦、确定性通过。
void test_pool_create_fails_when_down() {
    RedisConfig cfg;
    cfg.password_env = "";
    cfg.host = "127.0.0.1";
    cfg.port = 6390;  // 必死端口：没有任何服务监听
    cfg.connect_timeout = core::DurationMs{200};  // 快速失败，避免长阻塞
    auto p = ConnectionPool::Create(cfg);
    CHECK(!p.HasValue());
    CHECK(p.Err().Code() == core::ErrorCode::BUSY);
}

// ---- §17 集成：需真实实例；不可达则 SKIP ----
void test_integration_crud_and_ttl() {
    if (!RedisAvailable()) {
        ErrorFmt("SKIP test_integration_crud_and_ttl (no redis)\n");
        return;
    }
    RedisConfig cfg;
    cfg.password_env = "";
    auto cache = RedisCache::Create(cfg);
    CHECK(cache.HasValue());
    auto& c = *cache.Value();

    const Record rec = MakeRec("character:1", 1, "v1");
    CHECK(c.Put(rec).HasValue());
    auto got = c.Get("character:1");
    CHECK(got.HasValue() && got.Value().has_value());
    CHECK(got.Value().value().payload == "v1");
    CHECK(got.Value().value().version == 1);

    CHECK(c.Invalidate("character:1").HasValue());
    auto gone = c.Get("character:1");
    CHECK(gone.HasValue() && !gone.Value().has_value());

    // TTL 过期
    Record ttl_rec = MakeRec("ttl:k", 1, "tmp");
    CHECK(c.Put(ttl_rec, core::DurationMs{100}).HasValue());
    auto before = c.Get("ttl:k");
    CHECK(before.HasValue() && before.Value().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    auto after = c.Get("ttl:k");
    CHECK(after.HasValue() && !after.Value().has_value());
}

void test_integration_prefix_invalidation() {
    if (!RedisAvailable()) {
        ErrorFmt("SKIP test_integration_prefix_invalidation (no redis)\n");
        return;
    }
    RedisConfig cfg;
    cfg.password_env = "";
    auto cache = RedisCache::Create(cfg);
    CHECK(cache.HasValue());
    auto& c = *cache.Value();
    CHECK(c.Put(MakeRec("cache:char:1", 1, "a")).HasValue());
    CHECK(c.Put(MakeRec("cache:char:2", 1, "b")).HasValue());
    CHECK(c.Put(MakeRec("cache:inv:1", 1, "c")).HasValue());
    CHECK(c.InvalidatePrefix("cache:char:").HasValue());  // 走 SCAN，非全量键
    CHECK(c.Get("cache:char:1").Value().has_value() == false);
    CHECK(c.Get("cache:char:2").Value().has_value() == false);
    CHECK(c.Get("cache:inv:1").Value().has_value() == true);  // 其它前缀不受影响
}

}  // namespace

int main() {
    test_resolve_password_env();
    test_key_generation();
    test_serialization_roundtrip();
    test_serialization_corrupt();
    test_retry_classify();
    test_circuit_breaker_fsm();
    test_pool_create_fails_when_down();

    test_integration_crud_and_ttl();
    test_integration_prefix_invalidation();

    if (g_fail == 0) {
        ErrorFmt("DataService.Redis.Suite: ALL PASS\n");
        return 0;
    }
    ErrorFmt("DataService.Redis.Suite: %d FAIL\n", g_fail);
    return g_fail;
}
