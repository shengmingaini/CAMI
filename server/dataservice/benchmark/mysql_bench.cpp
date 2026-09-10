// server/dataservice/benchmark/mysql_bench.cpp
//
// TASK-028 §18 · MySQL 性能基准。输出机器可读 key=value 到 bench/mysql.txt。
// 用法：bin/mysql_bench --ops 10000 [--host=H] [--port=P] [--db=NAME]
//                        [--password-env=ENV_NAME]
//   --password-env 缺省为空 = 本地验证实例无密码（与 redis_bench 同口径）；
//   生产/带密码实例显式传 --password-env=MMORPG_MYSQL_PASSWORD（库层仍是严格语义：
//   名字给了但变量未设置 -> 明确失败，禁止空密码兜底）。
//
// 无实例时优雅退出（非零），不崩溃、不伪造指标（§20.7）。
// 指标口径（写路径统一走 blind upsert = VersionCheck{0,false}，保证 bench 可重复运行；
// 默认 VersionCheck{} 语义是「期望不存在」的 Insert，重复运行会撞 1062 重复键）：
//   insert_ns             单条 Save（参数化 UPSERT）平均耗时
//   select_ns             单条 Load 平均耗时（验收断言 ≤ 1e6 ns）
//   batch_insert_ns_per_1k 1,000 条 BatchSave 总耗时
//   pool_acquire_ns       连接池获取平均耗时（验收断言 ≤ 5000 ns）
//   txn_ns                100 条 BatchSaveAtomic 单事务平均耗时

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/core/config/config_manager.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/data/mysql/migration.h"
#include "mmo/data/mysql/mysql_client.h"
#include "mmo/data/mysql/mysql_config.h"
#include "mmo/data/mysql/mysql_store.h"
#include "mmo/data/record.h"

namespace core = mmo::core;
using namespace mmo::data;
using namespace mmo::data::mysql;

namespace {

std::uint64_t ElapsedNs(std::chrono::steady_clock::time_point t0) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
}

/// 准备表结构：EnsureDatabaseExists + 执行迁移（幂等）。
bool PrepareSchema(const ShardEndpoint& ep, const MySqlConfig& cfg, std::string_view password,
                   const std::string& migrations_dir) {
    auto created = EnsureDatabaseExists(ep, cfg, password);
    if (!created.HasValue()) {
        core::test::ErrorFmt("mysql_bench: ensure database failed: %.*s\n",
                             static_cast<int>(created.Err().Message().size()),
                             created.Err().Message().data());
        return false;
    }
    auto runner = MigrationRunner::Create(ep, cfg, password);
    if (!runner.HasValue()) {
        core::test::ErrorFmt("mysql_bench: migration connect failed: %.*s\n",
                             static_cast<int>(runner.Err().Message().size()),
                             runner.Err().Message().data());
        return false;
    }
    auto applied = runner.Value().Up(migrations_dir);
    if (!applied.HasValue()) {
        core::test::ErrorFmt("mysql_bench: migration failed: %.*s\n",
                             static_cast<int>(applied.Err().Message().size()),
                             applied.Err().Message().data());
        return false;
    }
    return true;
}

std::string MigrationsDir(std::string_view override_dir) {
    if (!override_dir.empty()) return std::string(override_dir);
    const char* root = std::getenv("MMO_PROJECT_ROOT");
    if (root != nullptr && root[0] != '\0') return std::string(root) + "/database/migrations";
    return "database/migrations";
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t ops = 10000;
    std::string host{"127.0.0.1"};
    std::uint16_t port = 3306;
    std::string db{"mmo_bench"};
    std::string migrations_dir;
    std::string password_env;  // 空 = 无密码（本地验证实例）

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool has_next = (i + 1 < argc);
        if (std::strcmp(a, "--ops") == 0 && has_next) {
            ops = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--host") == 0 && has_next) {
            host = argv[++i];
        } else if (std::strcmp(a, "--port") == 0 && has_next) {
            port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--db") == 0 && has_next) {
            db = argv[++i];
        } else if (std::strcmp(a, "--migrations") == 0 && has_next) {
            migrations_dir = argv[++i];
        } else if (std::strcmp(a, "--password-env") == 0 && has_next) {
            password_env = argv[++i];
        }
    }
    if (ops == 0) {
        core::test::Error("mysql_bench: --ops must be > 0\n");
        return 1;
    }

    MySqlConfig cfg;
    cfg.password_env = password_env;
    cfg.pool_size_per_shard = 8;
    cfg.query_timeout = core::DurationMs{3000};   // bench 放宽读写超时，避免偶发抖动误判
    cfg.acquire_timeout = core::DurationMs{1000};

    ShardEndpoint ep;
    ep.host = host;
    ep.port = port;
    ep.user = "root";
    ep.database = db;

    auto password = ResolveMySqlPassword(cfg);
    if (!password.HasValue()) {
        core::test::ErrorFmt("mysql_bench: cannot resolve password: %.*s\n",
                             static_cast<int>(password.Err().Message().size()),
                             password.Err().Message().data());
        return 2;
    }

    if (!PrepareSchema(ep, cfg, password.Value(), MigrationsDir(migrations_dir))) {
        core::test::Error("mysql_bench: cannot prepare schema (start MariaDB first)\n");
        return 2;
    }

    auto store = MySqlStore::CreateSingle(ep, cfg);
    if (!store.HasValue()) {
        core::test::Error("mysql_bench: cannot connect (start MariaDB first)\n");
        return 2;
    }
    auto& st = *store.Value();

    const std::string payload(96, 'y');  // 固定小对象（模拟扁平化后的角色快照）
    const auto domain = std::string{"bench"};

    // ---- 预热 + 单条插入 ----
    constexpr VersionCheck kBlind{0, false};  // 不校验版本，直接覆盖（可重复运行）

    const std::size_t warm = ops < 50 ? ops : 50;
    for (std::size_t i = 0; i < warm; ++i) {
        Record rec;
        rec.key = domain + ":" + std::to_string(i);
        rec.payload = payload;
        rec.version = 1;
        (void)st.Save(rec, kBlind);
    }

    std::uint64_t insert_ns_total = 0;
    for (std::size_t i = 0; i < ops; ++i) {
        Record rec;
        rec.key = domain + ":" + std::to_string(i);
        rec.payload = payload;
        rec.version = 1;
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = st.Save(rec, kBlind);
        insert_ns_total += ElapsedNs(t0);
        if (!r.HasValue()) {
            core::test::ErrorFmt("mysql_bench: save failed: %.*s\n",
                                 static_cast<int>(r.Err().Message().size()),
                                 r.Err().Message().data());
            return 3;
        }
    }
    const double insert_ns = static_cast<double>(insert_ns_total) / static_cast<double>(ops);

    // ---- 单条读取 ----
    std::uint64_t select_ns_total = 0;
    for (std::size_t i = 0; i < ops; ++i) {
        const DataKey key = domain + ":" + std::to_string(i);
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = st.Load(key);
        select_ns_total += ElapsedNs(t0);
        if (!r.HasValue()) {
            core::test::ErrorFmt("mysql_bench: load failed: %.*s\n",
                                 static_cast<int>(r.Err().Message().size()),
                                 r.Err().Message().data());
            return 3;
        }
    }
    const double select_ns = static_cast<double>(select_ns_total) / static_cast<double>(ops);

    // ---- 批量写入（1,000 条 / 轮）----
    constexpr std::size_t kBatch = 1000;
    constexpr int kBatchRounds = 5;
    double batch_insert_ns_per_1k = 0.0;
    {
        std::uint64_t total = 0;
        for (int r = 0; r < kBatchRounds; ++r) {
            std::vector<Record> recs(kBatch);
            for (std::size_t i = 0; i < kBatch; ++i) {
                recs[i].key = domain + ":batch:" + std::to_string(r * kBatch + i);
                recs[i].payload = payload;
                recs[i].version = 0;  // 0 = 不做版本校验（可重复运行，见文件头）
            }
            const auto t0 = std::chrono::steady_clock::now();
            const auto res = st.BatchSave(recs);
            total += ElapsedNs(t0);
            if (!res.HasValue()) {
                core::test::ErrorFmt("mysql_bench: batch save failed: %.*s\n",
                                     static_cast<int>(res.Err().Message().size()),
                                     res.Err().Message().data());
                return 3;
            }
        }
        batch_insert_ns_per_1k =
            static_cast<double>(total) / static_cast<double>(kBatchRounds);
    }

    // ---- 事务提交（100 条同分片原子批量）----
    constexpr std::size_t kTxnBatch = 100;
    constexpr int kTxnRounds = 20;
    double txn_ns = 0.0;
    {
        std::uint64_t total = 0;
        for (int r = 0; r < kTxnRounds; ++r) {
            std::vector<Record> recs(kTxnBatch);
            for (std::size_t i = 0; i < kTxnBatch; ++i) {
                recs[i].key = domain + ":txn:" + std::to_string(r * kTxnBatch + i);
                recs[i].payload = payload;
                recs[i].version = 0;  // 0 = 不做版本校验（可重复运行，见文件头）
            }
            const auto t0 = std::chrono::steady_clock::now();
            const auto res = st.BatchSaveAtomic(recs);
            total += ElapsedNs(t0);
            if (!res.HasValue()) {
                core::test::ErrorFmt("mysql_bench: atomic batch failed: %.*s\n",
                                     static_cast<int>(res.Err().Message().size()),
                                     res.Err().Message().data());
                return 3;
            }
        }
        txn_ns = static_cast<double>(total) / static_cast<double>(kTxnRounds);
    }

    // ---- 连接池获取延迟 ----
    double pool_acquire_ns = 0.0;
    {
        constexpr int kRounds = 5000;
        std::uint64_t total = 0;
        for (int i = 0; i < kRounds; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            auto lease = st.AcquireShard(0);
            total += ElapsedNs(t0);
            if (!lease.HasValue()) {
                core::test::ErrorFmt("mysql_bench: pool acquire failed: %.*s\n",
                                     static_cast<int>(lease.Err().Message().size()),
                                     lease.Err().Message().data());
                return 3;
            }
        }
        pool_acquire_ns = static_cast<double>(total) / static_cast<double>(kRounds);
    }

    const MySqlPoolStats ps = st.pool_stats(0);

    FILE* f = std::fopen("bench/mysql.txt", "w");
    if (f != nullptr) {
        std::fprintf(f, "insert_ns=%.3f\n", insert_ns);
        std::fprintf(f, "select_ns=%.3f\n", select_ns);
        std::fprintf(f, "batch_insert_ns_per_1k=%.3f\n", batch_insert_ns_per_1k);
        std::fprintf(f, "txn_ns=%.3f\n", txn_ns);
        std::fprintf(f, "pool_acquire_ns=%.3f\n", pool_acquire_ns);
        std::fprintf(f, "ops=%zu\n", ops);
        std::fprintf(f, "pool_size=%zu\n", cfg.pool_size_per_shard);
        std::fprintf(f, "conn_count=%zu\n", ps.in_use + ps.idle);
        std::fprintf(f, "slow_query_count=%llu\n",
                     static_cast<unsigned long long>(ps.slow_query_count));
        std::fclose(f);
    }
    core::test::LineFmt(
        "insert_ns=%.3f select_ns=%.3f batch_insert_ns_per_1k=%.3f txn_ns=%.3f "
        "pool_acquire_ns=%.3f ops=%zu\n",
        insert_ns, select_ns, batch_insert_ns_per_1k, txn_ns, pool_acquire_ns, ops);
    return 0;
}
