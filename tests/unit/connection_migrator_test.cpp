// 连接迁移治理器单测 [PROTOTYPE]
// 用 InMemoryTransport 仿真 TCP 基线 / QUIC 候选，覆盖：
//   1) 影子模式真实迁移走基线（候选不权威）
//   2) 影子评估正确计数候选成功 / 延迟
//   3) 达标后自主晋升 ActivePrimary
//   4) 候选连续失败触发熔断 → FallbackOnly + 告警
//   5) 活跃主下候选失败自动回退基线
//   6) FallbackOnly 强制回退基线
//
// 不依赖 Boost / 真实 socket，纯 STL，OFF 可编译。

#include "gateway/transport/connection_migrator.h"
#include "gateway/transport/in_memory_transport.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

using namespace cami::gateway::transport;
using namespace std::chrono_literals;

namespace {

ConnectionId make_cid(std::uint8_t v) {
    ConnectionId c{};
    for (std::size_t i = 0; i < c.bytes.size(); ++i) c.bytes[i] = v;
    return c;
}

// 便捷构造：仅覆盖晋升所需的最小影子评估次数（其余闸门用默认值）。
ConnectionMigrator::Config cfg_with(std::uint64_t promote_after) {
    ConnectionMigrator::Config c;
    c.promote_after_migrations = promote_after;
    return c;
}

int g_fail = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " #cond " @line " << __LINE__ << "\n";         \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

void test_shadow_routes_baseline() {
    InMemoryTransport base("tcp-baseline", /*healthy*/ true);
    InMemoryTransport cand("quic-candidate", /*healthy*/ true);
    ConnectionMigrator m(&base, &cand, cfg_with(1000));
    MigrationResult r = m.Migrate(make_cid(1), "ep");
    CHECK(r.success);
    CHECK(m.mode() == MigratorMode::Shadow);  // 尚未晋升
}

void test_shadow_evaluates_candidate() {
    InMemoryTransport base("tcp-baseline", true);
    InMemoryTransport cand("quic-candidate", true, 500us);
    ConnectionMigrator m(&base, &cand, cfg_with(1000));
    for (int i = 0; i < 10; ++i) m.Migrate(make_cid(static_cast<std::uint8_t>(i)), "ep");
    CHECK(m.stats().shadow_ops.load() == 10);
    CHECK(m.stats().candidate_ok.load() == 10);
    CHECK(m.stats().success_rate() == 1.0);
}

void test_promotion() {
    InMemoryTransport base("tcp-baseline", true);
    InMemoryTransport cand("quic-candidate", true, 500us);
    ConnectionMigrator m(&base, &cand, cfg_with(20));
    for (int i = 0; i < 25; ++i) m.Migrate(make_cid(static_cast<std::uint8_t>(i)), "ep");
    CHECK(m.mode() == MigratorMode::ActivePrimary);
}

void test_circuit_breaker() {
    InMemoryTransport base("tcp-baseline", true);
    InMemoryTransport cand("quic-candidate", /*healthy*/ false);
    std::string alert;
    ConnectionMigrator m(&base, &cand, cfg_with(1000));
    m.set_alert([&](const std::string& s) { alert += s; });
    for (int i = 0; i < 5; ++i) m.Migrate(make_cid(static_cast<std::uint8_t>(i)), "ep");
    CHECK(m.mode() == MigratorMode::FallbackOnly);
    CHECK(!alert.empty());
}

void test_active_primary_fallback() {
    InMemoryTransport base("tcp-baseline", true);
    InMemoryTransport cand("quic-candidate", true, 500us);
    ConnectionMigrator m(&base, &cand, cfg_with(5));
    for (int i = 0; i < 6; ++i) m.Migrate(make_cid(static_cast<std::uint8_t>(i)), "ep");  // 晋升
    CHECK(m.mode() == MigratorMode::ActivePrimary);
    cand.set_healthy(false);
    MigrationResult r = m.Migrate(make_cid(99), "ep");  // 候选失败 → 回退基线
    CHECK(r.success);                                   // 基线兜底成功
}

void test_fallback_only_always_baseline() {
    InMemoryTransport base("tcp-baseline", true);
    InMemoryTransport cand("quic-candidate", true);
    ConnectionMigrator m(&base, &cand, cfg_with(1000));
    m.trip_to_fallback("manual");
    MigrationResult r = m.Migrate(make_cid(7), "ep");
    CHECK(r.success);
    CHECK(m.mode() == MigratorMode::FallbackOnly);
}

}  // namespace

int main() {
    test_shadow_routes_baseline();
    test_shadow_evaluates_candidate();
    test_promotion();
    test_circuit_breaker();
    test_active_primary_fallback();
    test_fallback_only_always_baseline();

    if (g_fail == 0) {
        std::cout << "ALL connection_migrator TESTS PASSED\n";
        return 0;
    }
    std::cerr << g_fail << " CHECK(s) FAILED\n";
    return 1;
}
