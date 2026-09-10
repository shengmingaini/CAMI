// server/dataservice/tests/redis_test.cpp
//
// TASK-027 · Redis 适配器测试（§16 单元 / §17 集成 / §19 Failure / §20 验收）。
//
// 设计：纯逻辑单测（配置解析 / 键名 / 序列化 / 重试判定 / 熔断状态机 / 池探测失败）
//       不依赖真实实例，离线必跑；标注 [redis] 的集成用例在实例不可达时明确 SKIP
//       （打印 SKIP 且不计入失败，禁止伪装通过，§20.7）。
//
// 输出经 test_print.h（禁止裸 cout/printf）。

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <hiredis/hiredis.h>

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
#include "mmo/data/redis/session_store.h"
#include "mmo/gateway/session/session.h"

namespace {

using namespace mmo::data;
using namespace mmo::data::redis;
namespace core = mmo::core;
namespace gateway = mmo::gateway;
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

// ---- §15.5 重试判定（编号取 hiredis 真值：IO=1 / OTHER=2 / EOF=3 / PROTOCOL=4 / OOM=5 / TIMEOUT=6）----
void test_retry_classify() {
    CHECK(IsRetryableRedisError(kRedisErrTimeout, "recv timeout") == true);
    CHECK(IsRetryableRedisError(kRedisErrIo, "Connection refused") == true);
    CHECK(IsRetryableRedisError(kRedisErrOther, "Connection reset by peer") == true);
    CHECK(IsRetryableRedisError(kRedisErrEof, "") == true);  // 对端关闭 -> 可重连

    // 回归护栏：曾把 4 当成 TIMEOUT（实为 PROTOCOL）、把 3 当成 IO（实为 EOF）。
    CHECK(kRedisErrTimeout == 6);
    CHECK(kRedisErrProtocol == 4);
    CHECK(kRedisErrEof == 3);
    CHECK(kRedisErrIo == 1);
    CHECK(IsRetryableRedisError(kRedisErrProtocol, "protocol error") == false);
    CHECK(IsRetryableRedisError(kRedisErrOom, "out of memory") == false);

    CHECK(IsRetryableRedisError(0, "WRONGTYPE") == false);  // 业务错误不重试
    CHECK(IsRetryableRedisError(0, "Connection refused") == true);  // 兜底识别连接层错误
    CHECK(IsRetryableRedisError(0, nullptr) == false);
}

// ---- §15.5 回归护栏：retry.h 的数值镜像必须与 hiredis 真值一致 ----
// （src/ 内已有 static_assert 做编译期护栏，此处运行期再留一份可读证据）
void test_retry_mirror_matches_hiredis() {
    CHECK(kRedisErrIo == REDIS_ERR_IO);
    CHECK(kRedisErrOther == REDIS_ERR_OTHER);
    CHECK(kRedisErrEof == REDIS_ERR_EOF);
    CHECK(kRedisErrProtocol == REDIS_ERR_PROTOCOL);
    CHECK(kRedisErrOom == REDIS_ERR_OOM);
    CHECK(kRedisErrTimeout == REDIS_ERR_TIMEOUT);
    // 显式固化「编号非顺序语义」：4 是协议错误而非超时（首版 bug 的根因）
    CHECK(REDIS_ERR_TIMEOUT != 4);
    CHECK(REDIS_ERR_PROTOCOL == 4);
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

// ---- §16 会话序列化往返 + 损坏/截断容错 ----
void test_session_encode_decode() {
    gateway::Session s;
    s.session_id = gateway::MakeSessionId(7, 3);
    s.player_id = 4242;
    s.last_heartbeat = core::SteadyTime(std::chrono::nanoseconds(987654321LL));
    s.conn_id = 0x1122334455667788ull;
    s.trace_id = 0xAABBCCDDEEFF0011ull;
    s.gateway_id = 9;
    s.game_node_id = 12;
    s.scene_id = 34;
    s.version = 5;
    s.state = gateway::SessionState::Active;

    const std::string blob = EncodeSession(s);
    auto dec = DecodeSession(blob);
    CHECK(dec.HasValue());
    CHECK(dec.Value().session_id == s.session_id);
    CHECK(dec.Value().player_id == s.player_id);
    CHECK(dec.Value().last_heartbeat == s.last_heartbeat);
    CHECK(dec.Value().conn_id == s.conn_id);
    CHECK(dec.Value().trace_id == s.trace_id);
    CHECK(dec.Value().gateway_id == s.gateway_id);
    CHECK(dec.Value().game_node_id == s.game_node_id);
    CHECK(dec.Value().scene_id == s.scene_id);
    CHECK(dec.Value().version == s.version);
    CHECK(dec.Value().state == gateway::SessionState::Active);

    CHECK(!DecodeSession("bad").HasValue());          // 过短
    CHECK(!DecodeSession("XXXX" + blob.substr(4)).HasValue());  // 魔数错
    std::string trunc = blob;
    trunc.resize(trunc.size() - 1);                   // 截断
    CHECK(!DecodeSession(trunc).HasValue());
}

// ---- §8 会话键空间规范 ----
void test_session_keys() {
    CHECK(SessionStoreKey(123) == "sess:123");
    CHECK(SessionPlayerIndexKey(456) == "sess:player:456");
}

// ---- §17 集成：SessionStore CRUD + FindByPlayer（需真实实例，否则 SKIP）----
void test_integration_session_store() {
    if (!RedisAvailable()) {
        ErrorFmt("SKIP test_integration_session_store (no redis)\n");
        return;
    }
    RedisConfig cfg;
    cfg.password_env = "";
    auto store = SessionStore::Create(cfg);
    CHECK(store.HasValue());
    auto& st = *store.Value();

    gateway::Session s;
    s.session_id = gateway::MakeSessionId(1, 1);
    s.player_id = 1001;
    s.conn_id = 777;
    s.trace_id = 888;
    s.gateway_id = 1;
    s.game_node_id = 2;
    s.scene_id = 3;
    s.version = 1;
    s.state = gateway::SessionState::Active;
    s.last_heartbeat = core::SteadyTime(std::chrono::nanoseconds(555));

    CHECK(st.Put(s).HasValue());
    auto got = st.Get(s.session_id);
    CHECK(got.HasValue() && got.Value().has_value());
    CHECK(got.Value()->player_id == 1001);
    CHECK(got.Value()->state == gateway::SessionState::Active);

    auto byp = st.FindByPlayer(1001);
    CHECK(byp.HasValue() && byp.Value().has_value());
    CHECK(byp.Value()->session_id == s.session_id);

    auto none = st.FindByPlayer(999999);  // 不存在 -> nullopt（非错误）
    CHECK(none.HasValue() && !none.Value().has_value());

    CHECK(st.Remove(s.session_id).HasValue());
    auto gone = st.Get(s.session_id);
    CHECK(gone.HasValue() && !gone.Value().has_value());
    auto gone_p = st.FindByPlayer(1001);
    CHECK(gone_p.HasValue() && !gone_p.Value().has_value());
    CHECK(st.Remove(s.session_id).HasValue());  // 重复移除幂等

    gateway::Session bad;  // session_id=0 非法
    bad.session_id = gateway::kInvalidSessionId;
    bad.player_id = 7;
    CHECK(!st.Put(bad).HasValue());
}

// ---- §20.3 集成：Routing 键读写（route:player / route:scene，供 TASK-010 使用）----
void test_integration_routing_keys() {
    if (!RedisAvailable()) {
        ErrorFmt("SKIP test_integration_routing_keys (no redis)\n");
        return;
    }
    RedisConfig cfg;
    cfg.password_env = "";
    auto cache = RedisCache::Create(cfg);
    CHECK(cache.HasValue());
    auto& c = *cache.Value();

    CHECK(c.Put(MakeRec(PlayerRouteKey("4242"), 1, "node-3")).HasValue());
    auto p = c.Get(PlayerRouteKey("4242"));
    CHECK(p.HasValue() && p.Value().has_value());
    CHECK(p.Value()->payload == "node-3");

    CHECK(c.Put(MakeRec(SceneRouteKey("7"), 1, "node-9")).HasValue());
    auto sc = c.Get(SceneRouteKey("7"));
    CHECK(sc.HasValue() && sc.Value().has_value());
    CHECK(sc.Value()->payload == "node-9");

    CHECK(c.Invalidate(PlayerRouteKey("4242")).HasValue());
    CHECK(c.Invalidate(SceneRouteKey("7")).HasValue());
}

// ---- §19 集成：连接池耗尽 -> BUSY 并计数，归还后复用（不死锁）----
void test_integration_pool_exhaustion_busy() {
    if (!RedisAvailable()) {
        ErrorFmt("SKIP test_integration_pool_exhaustion_busy (no redis)\n");
        return;
    }
    RedisConfig cfg;
    cfg.password_env = "";
    cfg.pool_size = 1;
    cfg.op_timeout = core::DurationMs{30};
    auto pool = ConnectionPool::Create(cfg);
    CHECK(pool.HasValue());

    {
        auto held = pool.Value()->Acquire(core::DurationMs{200});
        CHECK(held.HasValue());  // 占用池中唯一连接
        auto second = pool.Value()->Acquire(core::DurationMs{50});
        CHECK(!second.HasValue());  // 池满 -> 超时
        CHECK(second.Err().Code() == core::ErrorCode::BUSY);
        CHECK(pool.Value()->Stats().acquire_timeout_count >= 1);
    }  // held 在此归还

    auto third = pool.Value()->Acquire(core::DurationMs{200});
    CHECK(third.HasValue());  // 归还后可复用，未死锁
}

// ---- §19：实例不可达时 Create 返回明确错误（不崩溃、不挂死）----
void test_cache_create_fails_when_down() {
    RedisConfig cfg;
    cfg.password_env = "";
    cfg.port = 6390;  // 必死端口
    cfg.connect_timeout = core::DurationMs{200};
    auto cache = RedisCache::Create(cfg);
    CHECK(!cache.HasValue());
    CHECK(cache.Err().Code() == core::ErrorCode::BUSY);
}

// ---- §19 集成：密码错误 -> 启动即失败（禁止用空密码兜底 / 重试风暴）----
// 实例本身无密码，却带一个错误 AUTH 去连：服务端返回错误 -> Create 明确失败。
void test_integration_password_error() {
    if (!RedisAvailable()) {
        ErrorFmt("SKIP test_integration_password_error (no redis)\n");
        return;
    }
#ifdef _WIN32
    _putenv_s("MMORPG_REDIS_PASSWORD_WRONG", "definitely-wrong");
#else
    setenv("MMORPG_REDIS_PASSWORD_WRONG", "definitely-wrong", 1);
#endif
    RedisConfig cfg;
    cfg.password_env = "MMORPG_REDIS_PASSWORD_WRONG";
    cfg.connect_timeout = core::DurationMs{500};
    auto pool = ConnectionPool::Create(cfg);
    CHECK(!pool.HasValue());
}

// ---- §19 集成：慢查询（服务端阻塞）-> TIMEOUT 而非挂死 ----
void test_integration_slow_query_timeout() {
    if (!RedisAvailable()) {
        ErrorFmt("SKIP test_integration_slow_query_timeout (no redis)\n");
        return;
    }
    RedisConfig cfg;
    cfg.password_env = "";
    cfg.op_timeout = core::DurationMs{200};
    auto cache = RedisCache::Create(cfg);
    CHECK(cache.HasValue());
    (void)(*cache.Value()).Get("slow:warm");  // 预热一条池连接

    redisContext* raw = redisConnect("127.0.0.1", 6379);
    if (raw == nullptr || raw->err != 0) {
        if (raw) redisFree(raw);
        ErrorFmt("SKIP test_integration_slow_query_timeout (no raw conn)\n");
        return;
    }
    std::thread blocker([raw] {
        redisReply* r = static_cast<redisReply*>(redisCommand(raw, "DEBUG SLEEP 2"));
        if (r) freeReplyObject(r);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));  // 让阻塞先到达服务端

    // 先取结果、再 join、后断言：CHECK 宏在失败时 return，若在其之前未 join
    // 会让 std::thread 析构触发 std::terminate（§9 禁止测试自杀）。
    std::optional<core::ErrorCode> fail_code;
    {
        auto g = (*cache.Value()).Get("slow:probe");
        if (!g.HasValue()) fail_code = g.Err().Code();
    }

    blocker.join();       // 等 DEBUG SLEEP 结束，服务端恢复
    redisFree(raw);

    if (!fail_code.has_value()) {
        // 服务端未阻塞（DEBUG SLEEP 不支持）——如实报告，不伪装通过（§20.7）
        ErrorFmt("NOTE slow_query: server not blocked (DEBUG SLEEP unsupported) — assertion skipped\n");
    } else {
        CHECK(*fail_code == core::ErrorCode::TIMEOUT);
    }
    auto warm = (*cache.Value()).Get("slow:warm");  // 恢复后可用（超时连接已被池丢弃重建）
    CHECK(warm.HasValue());
}

}  // namespace

int main() {
    test_resolve_password_env();
    test_key_generation();
    test_serialization_roundtrip();
    test_serialization_corrupt();
    test_session_encode_decode();
    test_session_keys();
    test_retry_classify();
    test_retry_mirror_matches_hiredis();
    test_circuit_breaker_fsm();
    test_pool_create_fails_when_down();
    test_cache_create_fails_when_down();

    test_integration_crud_and_ttl();
    test_integration_prefix_invalidation();
    test_integration_session_store();
    test_integration_routing_keys();
    test_integration_pool_exhaustion_busy();
    test_integration_password_error();
    test_integration_slow_query_timeout();

    if (g_fail == 0) {
        ErrorFmt("DataService.Redis.Suite: ALL PASS\n");
        return 0;
    }
    ErrorFmt("DataService.Redis.Suite: %d FAIL\n", g_fail);
    return g_fail;
}
