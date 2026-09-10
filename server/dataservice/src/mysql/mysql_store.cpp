// server/dataservice/src/mysql/mysql_store.cpp
//
// TASK-028 §7 / §15.4 / §15.5 · MySQL 权威存储实现。
//
// IDataStore 的业务无关落地：`<domain>:<id>` -> 分片 -> 该分片库内 kv_store 一行。
// 乐观锁：version 列即锁（§20.4）。事务：只在单分片内发起（§9 / §21）。
//
// 关于 updated_at：Record.updated_at 是**单调时钟**时刻，无法从数据库墙钟时间还原，
// 故 Load 时置为当前单调时刻（TASK-026 已声明该字段「仅用于审计/调试」）；
// 数据库侧的 updated_at 列按墙钟维护，供运维查询。

#include "mmo/data/mysql/mysql_store.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mmo/core/time/clock.h"
#include "mmo/data/mysql/migration.h"
#include "mmo/data/mysql/sql_builder.h"

namespace mmo::data::mysql {

namespace {

constexpr char kKvKeyColumn[] = "k";

core::Error Err(core::ErrorCode code, std::string_view msg) {
    return core::Error(code, msg, core::domain::kData);
}

std::uint64_t ElapsedNs(std::chrono::steady_clock::time_point t0) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count());
}

/// 一次写入所需的语句与参数（按 SaveMode 分派）。
struct SaveStmt {
    std::string sql;
    MySqlParams params;
    bool guarded{false};
};

core::Result<SaveStmt> MakeSaveStmt(const mmo::data::Record& rec, const mmo::data::VersionCheck& vc) {
    SaveStmt out;
    core::Result<std::string> sql = core::Result<std::string>::Ok(std::string{});
    switch (DecideSaveMode(vc)) {
        case SaveMode::UpdateGuarded:
            sql = BuildGuardedUpdateSql(tables::kKvStore, kKvKeyColumn);
            out.params = {MySqlValue::Blob(rec.payload), MySqlValue::Uint(rec.version),
                          MySqlValue::Text(rec.key), MySqlValue::Uint(vc.expected_version)};
            out.guarded = true;
            break;
        case SaveMode::Insert:
            sql = BuildInsertSql(tables::kKvStore, kKvKeyColumn);
            out.params = {MySqlValue::Text(rec.key), MySqlValue::Blob(rec.payload),
                          MySqlValue::Uint(rec.version)};
            break;
        case SaveMode::Upsert:
            sql = BuildUpsertSql(tables::kKvStore, kKvKeyColumn);
            out.params = {MySqlValue::Text(rec.key), MySqlValue::Blob(rec.payload),
                          MySqlValue::Uint(rec.version)};
            break;
    }
    if (!sql.HasValue()) return core::Result<SaveStmt>::Fail(sql.Err());
    out.sql = std::move(sql.Value());
    return core::Result<SaveStmt>::Ok(std::move(out));
}

/// 批量写按 TASK-026 冻结语义决定每条的写模式（与 InMemoryStore::BatchSave 对齐）：
///   version == 0 -> 不做版本校验，直接覆盖（Upsert）
///   version != 0 -> 期望实际版本 == rec.version 的乐观锁更新（行不存在即冲突）
mmo::data::VersionCheck BatchVersionCheck(const mmo::data::Record& rec) {
    return rec.version == 0u ? mmo::data::VersionCheck{0u, false}
                             : mmo::data::VersionCheck{rec.version, true};
}

/// 由一行 kv_store 结果还原 Record（列顺序：payload, version）。
mmo::data::Record RecordFromRow(const mmo::data::DataKey& key, const MySqlRow& row) {
    mmo::data::Record rec;
    rec.key = key;
    const auto payload = row.Text(0);
    if (payload.has_value()) rec.payload = *payload;
    const auto ver = row.Int(1);
    if (ver.has_value() && *ver > 0) rec.version = static_cast<std::uint32_t>(*ver);
    rec.updated_at = mmo::core::MonotonicClock::Point();  // 单调时刻不可从墙钟还原（见文件头）
    return rec;
}

}  // namespace

// ============================ 构造/析构 ============================

MySqlStore::MySqlStore(ShardRouter router, MySqlConfig cfg,
                       std::vector<std::unique_ptr<MySqlConnectionPool>> pools)
    : router_(std::move(router)), cfg_(std::move(cfg)), pools_(std::move(pools)) {}

MySqlStore::~MySqlStore() = default;

core::Result<std::unique_ptr<MySqlStore>> MySqlStore::Create(const ShardConfig& shard_cfg,
                                                             const MySqlConfig& cfg) {
    auto router = ShardRouter::Create(shard_cfg, cfg.endpoints);
    if (!router.HasValue()) {
        return core::Result<std::unique_ptr<MySqlStore>>::Fail(router.Err());
    }
    std::vector<std::unique_ptr<MySqlConnectionPool>> pools;
    pools.reserve(cfg.endpoints.size());
    for (const auto& ep : cfg.endpoints) {
        auto pool = MySqlConnectionPool::Create(ep, cfg);
        if (!pool.HasValue()) {
            // 任一分片不可达即明确失败：禁止静默少建分片（否则路由会指向不存在的池）
            return core::Result<std::unique_ptr<MySqlStore>>::Fail(pool.Err());
        }
        pools.push_back(std::move(pool).Value());
    }
    std::unique_ptr<MySqlStore> store(
        new MySqlStore(std::move(router).Value(), cfg, std::move(pools)));

    auto ddl = BuildCreateKvStoreSql();
    if (!ddl.HasValue()) {
        return core::Result<std::unique_ptr<MySqlStore>>::Fail(ddl.Err());
    }
    for (std::size_t i = 0; i < store->pools_.size(); ++i) {
        const auto shard = static_cast<std::uint32_t>(i);
        auto lease = store->AcquireShard(shard);
        if (!lease.HasValue()) {
            return core::Result<std::unique_ptr<MySqlStore>>::Fail(lease.Err());
        }
        auto rs = lease.Value().handle()->Execute(ddl.Value());
        if (!rs.HasValue()) {
            return core::Result<std::unique_ptr<MySqlStore>>::Fail(rs.Err());
        }
    }
    return core::Result<std::unique_ptr<MySqlStore>>::Ok(std::move(store));
}

core::Result<std::unique_ptr<MySqlStore>> MySqlStore::CreateSingle(const ShardEndpoint& ep,
                                                                  const MySqlConfig& cfg) {
    ShardConfig sc;
    sc.shard_count = 1;
    MySqlConfig c = cfg;
    c.endpoints = {ep};
    return Create(sc, c);
}

core::Result<MySqlConnectionPool*> MySqlStore::PoolOf(std::uint32_t shard) const {
    if (shard >= pools_.size()) {
        return core::Result<MySqlConnectionPool*>::Fail(
            Err(core::ErrorCode::INTERNAL_ERROR, "shard out of range"));
    }
    return core::Result<MySqlConnectionPool*>::Ok(pools_[shard].get());
}

core::Result<MySqlLease> MySqlStore::AcquireShard(std::uint32_t shard) {
    auto pool = PoolOf(shard);
    if (!pool.HasValue()) return core::Result<MySqlLease>::Fail(pool.Err());
    return pool.Value()->Acquire(cfg_.acquire_timeout);
}

MySqlPoolStats MySqlStore::pool_stats(std::uint32_t shard) const {
    if (shard >= pools_.size()) return MySqlPoolStats{};
    return pools_[shard]->Stats();
}

HealthStatus MySqlStore::Health(std::uint32_t shard) const noexcept {
    if (shard >= pools_.size()) return HealthStatus::Unavailable;
    return pools_[shard]->Health();
}

void MySqlStore::NoteStatement(std::uint32_t shard, std::string_view sql, std::uint64_t ns) {
    if (shard < pools_.size()) pools_[shard]->NoteStatement(sql, ns);
}

bool MySqlStore::IsKnownTable(std::string_view table) noexcept {
    return table == tables::kKvStore || table == tables::kSchemaMigrations ||
           table == tables::kAccount || table == tables::kCharacter ||
           table == tables::kInventory || table == tables::kEquipment ||
           table == tables::kQuest || table == tables::kGuild || table == tables::kMail;
}

// ============================ IDataStore ============================

core::Result<std::optional<mmo::data::Record>> MySqlStore::Load(const mmo::data::DataKey& key) {
    auto parts = ShardRouter::ParseKey(key);
    if (!parts.HasValue()) return core::Result<std::optional<mmo::data::Record>>::Fail(parts.Err());

    const std::uint32_t shard = router_.ShardOf(parts.Value().entity_id);
    auto lease = AcquireShard(shard);
    if (!lease.HasValue()) {
        return core::Result<std::optional<mmo::data::Record>>::Fail(lease.Err());
    }
    auto sql = BuildSelectSql(tables::kKvStore, kKvKeyColumn);
    if (!sql.HasValue()) {
        return core::Result<std::optional<mmo::data::Record>>::Fail(sql.Err());
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto rs = lease.Value().handle()->ExecuteParams(sql.Value(), MySqlParams{MySqlValue::Text(key)});
    NoteStatement(shard, sql.Value(), ElapsedNs(t0));
    if (!rs.HasValue()) return core::Result<std::optional<mmo::data::Record>>::Fail(rs.Err());

    if (rs.Value().rows.empty()) {
        return core::Result<std::optional<mmo::data::Record>>::Ok(
            std::optional<mmo::data::Record>(std::nullopt));
    }
    if (rs.Value().rows.front().values.size() < 2) {
        return core::Result<std::optional<mmo::data::Record>>::Fail(
            Err(core::ErrorCode::INTERNAL_ERROR, "kv_store row shape mismatch"));
    }
    return core::Result<std::optional<mmo::data::Record>>::Ok(
        std::optional<mmo::data::Record>(RecordFromRow(key, rs.Value().rows.front())));
}

core::Result<void> MySqlStore::Save(const mmo::data::Record& rec, mmo::data::VersionCheck vc) {
    auto parts = ShardRouter::ParseKey(rec.key);
    if (!parts.HasValue()) return core::Result<void>::Fail(parts.Err());

    const std::uint32_t shard = router_.ShardOf(parts.Value().entity_id);
    auto lease = AcquireShard(shard);
    if (!lease.HasValue()) return core::Result<void>::Fail(lease.Err());

    auto stmt_r = MakeSaveStmt(rec, vc);
    if (!stmt_r.HasValue()) return core::Result<void>::Fail(stmt_r.Err());
    const SaveStmt& stmt = stmt_r.Value();

    const auto t0 = std::chrono::steady_clock::now();
    auto rs = lease.Value().handle()->ExecuteParams(stmt.sql, stmt.params);
    NoteStatement(shard, stmt.sql, ElapsedNs(t0));
    if (!rs.HasValue()) return core::Result<void>::Fail(rs.Err());

    if (stmt.guarded && rs.Value().affected_rows == 0) {
        // UPDATE ... WHERE version = expected 影响 0 行：版本不符（或行不存在）
        return core::Result<void>::Fail(
            Err(core::ErrorCode::VERSION_CONFLICT, "mysql store: version mismatch"));
    }
    return core::Result<void>::Ok();
}

core::Result<void> MySqlStore::Delete(const mmo::data::DataKey& key, mmo::data::VersionCheck vc) {
    auto parts = ShardRouter::ParseKey(key);
    if (!parts.HasValue()) return core::Result<void>::Fail(parts.Err());

    const std::uint32_t shard = router_.ShardOf(parts.Value().entity_id);
    auto lease = AcquireShard(shard);
    if (!lease.HasValue()) return core::Result<void>::Fail(lease.Err());

    const SaveMode mode = DecideSaveMode(vc);
    auto sql = (mode == SaveMode::UpdateGuarded)
                   ? BuildGuardedDeleteSql(tables::kKvStore, kKvKeyColumn)
                   : BuildPlainDeleteSql(tables::kKvStore, kKvKeyColumn);
    if (!sql.HasValue()) return core::Result<void>::Fail(sql.Err());

    MySqlParams params{MySqlValue::Text(key)};
    if (mode == SaveMode::UpdateGuarded) params.push_back(MySqlValue::Uint(vc.expected_version));

    const auto t0 = std::chrono::steady_clock::now();
    auto rs = lease.Value().handle()->ExecuteParams(sql.Value(), params);
    NoteStatement(shard, sql.Value(), ElapsedNs(t0));
    if (!rs.HasValue()) return core::Result<void>::Fail(rs.Err());

    if (mode == SaveMode::UpdateGuarded && rs.Value().affected_rows == 0) {
        // 0 行受影响有两种含义，必须区分（与 TASK-026 InMemoryStore 冻结语义一致）：
        //   · 键不存在 + expected_version == 0 -> 幂等成功（删除不存在的键不是错误）
        //   · 键存在但版本不符 -> VERSION_CONFLICT
        auto exists = lease.Value().handle()->ExecuteParams(
            BuildSelectSql(tables::kKvStore, kKvKeyColumn).Value(),
            MySqlParams{MySqlValue::Text(key)});
        if (!exists.HasValue()) return core::Result<void>::Fail(exists.Err());
        if (!exists.Value().rows.empty()) {
            return core::Result<void>::Fail(
                Err(core::ErrorCode::VERSION_CONFLICT, "mysql store: version mismatch on delete"));
        }
    }
    return core::Result<void>::Ok();
}

core::Result<std::vector<mmo::data::Record>> MySqlStore::BatchLoad(
    std::span<const mmo::data::DataKey> keys) {
    std::vector<mmo::data::Record> out;
    if (keys.empty()) return core::Result<std::vector<mmo::data::Record>>::Ok(std::move(out));

    // 按分片分组，每分片一条 IN (...) —— 避免 N+1 往返（单分片内 1000 条一次往返）
    std::unordered_map<std::uint32_t, std::vector<const mmo::data::DataKey*>> groups;
    for (const auto& key : keys) {
        auto parts = ShardRouter::ParseKey(key);
        if (!parts.HasValue()) {
            return core::Result<std::vector<mmo::data::Record>>::Fail(parts.Err());
        }
        groups[router_.ShardOf(parts.Value().entity_id)].push_back(&key);
    }

    for (const auto& [shard, ks] : groups) {
        auto sql = BuildBatchSelectSql(tables::kKvStore, kKvKeyColumn, ks.size());
        if (!sql.HasValue()) {
            return core::Result<std::vector<mmo::data::Record>>::Fail(sql.Err());
        }
        MySqlParams params;
        params.reserve(ks.size());
        for (const auto* k : ks) params.push_back(MySqlValue::Text(*k));

        auto lease = AcquireShard(shard);
        if (!lease.HasValue()) {
            return core::Result<std::vector<mmo::data::Record>>::Fail(lease.Err());
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto rs = lease.Value().handle()->ExecuteParams(sql.Value(), params);
        NoteStatement(shard, sql.Value(), ElapsedNs(t0));
        if (!rs.HasValue()) {
            return core::Result<std::vector<mmo::data::Record>>::Fail(rs.Err());
        }
        for (const auto& row : rs.Value().rows) {
            const auto key_text = row.Text(0);
            if (!key_text.has_value()) continue;  // 主键非空，理论不可达
            mmo::data::Record rec;
            rec.key = *key_text;
            const auto payload = row.Text(1);
            if (payload.has_value()) rec.payload = *payload;
            const auto ver = row.Int(2);
            if (ver.has_value() && *ver > 0) rec.version = static_cast<std::uint32_t>(*ver);
            rec.updated_at = mmo::core::MonotonicClock::Point();
            out.push_back(std::move(rec));
        }
    }
    return core::Result<std::vector<mmo::data::Record>>::Ok(std::move(out));
}

core::Result<std::vector<mmo::data::BatchOutcome>> MySqlStore::BatchSave(
    std::span<const mmo::data::Record> recs) {
    std::vector<mmo::data::BatchOutcome> outcomes;
    outcomes.reserve(recs.size());
    if (recs.empty()) {
        return core::Result<std::vector<mmo::data::BatchOutcome>>::Ok(std::move(outcomes));
    }

    // 按分片分组（跨分片只做「每分片各自事务」，绝不发起跨分片事务）
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < recs.size(); ++i) {
        auto parts = ShardRouter::ParseKey(recs[i].key);
        if (!parts.HasValue()) {
            return core::Result<std::vector<mmo::data::BatchOutcome>>::Fail(parts.Err());
        }
        groups[router_.ShardOf(parts.Value().entity_id)].push_back(i);
    }

    outcomes.resize(recs.size());
    for (std::size_t i = 0; i < recs.size(); ++i) outcomes[i].key = recs[i].key;

    for (const auto& [shard, idxs] : groups) {
        auto lease = AcquireShard(shard);
        if (!lease.HasValue()) {
            for (const std::size_t i : idxs) {
                outcomes[i].ok = false;
                outcomes[i].code = static_cast<std::uint32_t>(lease.Err().Code());
            }
            continue;
        }
        MySqlConnection* conn = lease.Value().handle();
        auto begin = conn->Begin();
        if (!begin.HasValue()) {
            for (const std::size_t i : idxs) {
                outcomes[i].ok = false;
                outcomes[i].code = static_cast<std::uint32_t>(begin.Err().Code());
            }
            continue;
        }

        bool txn_poisoned = false;
        core::Error first_txn_err = Err(core::ErrorCode::INTERNAL_ERROR, "mysql txn failed");
        for (const std::size_t i : idxs) {
            if (txn_poisoned) {
                outcomes[i].ok = false;
                outcomes[i].code = static_cast<std::uint32_t>(first_txn_err.Code());
                continue;
            }
            auto stmt = MakeSaveStmt(recs[i], BatchVersionCheck(recs[i]));
            if (!stmt.HasValue()) {
                outcomes[i].ok = false;
                outcomes[i].code = static_cast<std::uint32_t>(stmt.Err().Code());
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            auto rs = conn->ExecuteParams(stmt.Value().sql, stmt.Value().params);
            NoteStatement(shard, stmt.Value().sql, ElapsedNs(t0));
            if (!rs.HasValue()) {
                outcomes[i].ok = false;
                outcomes[i].code = static_cast<std::uint32_t>(rs.Err().Code());
                // 连接类/死锁错误会污染事务 -> 整体回滚；数据类错误（如重复键）不影响其它条目
                const std::uint32_t code = outcomes[i].code;
                const bool fatal = code == static_cast<std::uint32_t>(core::ErrorCode::BUSY) ||
                                   code == static_cast<std::uint32_t>(core::ErrorCode::TIMEOUT);
                if (fatal) {
                    txn_poisoned = true;
                    first_txn_err = rs.Err();
                }
                continue;
            }
            if (stmt.Value().guarded && rs.Value().affected_rows == 0) {
                // 乐观锁未命中：逐条失败，不污染事务（其它条目仍应提交）
                outcomes[i].ok = false;
                outcomes[i].code = static_cast<std::uint32_t>(core::ErrorCode::VERSION_CONFLICT);
                continue;
            }
            outcomes[i].ok = true;
            outcomes[i].code = static_cast<std::uint32_t>(core::ErrorCode::OK);
        }

        if (txn_poisoned) {
            (void)conn->Rollback();
        } else {
            auto commit = conn->Commit();
            if (!commit.HasValue()) {
                (void)conn->Rollback();
                for (const std::size_t i : idxs) {
                    outcomes[i].ok = false;
                    outcomes[i].code = static_cast<std::uint32_t>(commit.Err().Code());
                }
            }
        }
    }
    return core::Result<std::vector<mmo::data::BatchOutcome>>::Ok(std::move(outcomes));
}

core::Result<std::uint32_t> RequireSingleShard(const ShardRouter& router,
                                               std::span<const mmo::data::DataKey> keys) {
    if (keys.empty()) {
        return core::Result<std::uint32_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "empty key set has no shard"));
    }
    std::optional<std::uint32_t> shard;
    for (const auto& key : keys) {
        auto parts = ShardRouter::ParseKey(key);
        if (!parts.HasValue()) return core::Result<std::uint32_t>::Fail(parts.Err());
        const std::uint32_t s = router.ShardOf(parts.Value().entity_id);
        if (!shard.has_value()) {
            shard = s;
        } else if (*shard != s) {
            // §21：跨分片事务第一版不支持，必须明确报错（禁止静默拆成多次事务）
            return core::Result<std::uint32_t>::Fail(
                Err(core::ErrorCode::INVALID_ARGUMENT, "cross-shard transaction is not supported"));
        }
    }
    return core::Result<std::uint32_t>::Ok(*shard);
}

core::Result<void> MySqlStore::BatchSaveAtomic(std::span<const mmo::data::Record> recs) {
    if (recs.empty()) return core::Result<void>::Ok();

    std::vector<mmo::data::DataKey> keys;
    keys.reserve(recs.size());
    for (const auto& rec : recs) keys.push_back(rec.key);

    auto shard = RequireSingleShard(router_, keys);
    if (!shard.HasValue()) return core::Result<void>::Fail(shard.Err());

    auto lease = AcquireShard(shard.Value());
    if (!lease.HasValue()) return core::Result<void>::Fail(lease.Err());
    MySqlConnection* conn = lease.Value().handle();

    auto begin = conn->Begin();
    if (!begin.HasValue()) return begin;
    for (const auto& rec : recs) {
        auto stmt = MakeSaveStmt(rec, BatchVersionCheck(rec));
        if (!stmt.HasValue()) {
            (void)conn->Rollback();
            return core::Result<void>::Fail(stmt.Err());
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto rs = conn->ExecuteParams(stmt.Value().sql, stmt.Value().params);
        NoteStatement(shard.Value(), stmt.Value().sql, ElapsedNs(t0));
        if (!rs.HasValue()) {
            (void)conn->Rollback();  // 任一条失败整体回滚（§15.5）
            return core::Result<void>::Fail(rs.Err());
        }
        if (stmt.Value().guarded && rs.Value().affected_rows == 0) {
            (void)conn->Rollback();  // 乐观锁未命中：整批回滚（原子语义）
            return core::Result<void>::Fail(
                Err(core::ErrorCode::VERSION_CONFLICT, "atomic batch: version mismatch"));
        }
    }
    return conn->Commit();
}

core::Result<std::vector<std::uint32_t>> MySqlStore::Migrate(std::string_view migrations_dir) {
    auto pw = ResolveMySqlPassword(cfg_);
    if (!pw.HasValue()) return core::Result<std::vector<std::uint32_t>>::Fail(pw.Err());

    std::vector<std::uint32_t> applied;
    for (std::size_t i = 0; i < pools_.size(); ++i) {
        const ShardEndpoint& ep = pools_[i]->endpoint();
        auto runner = MigrationRunner::Create(ep, cfg_, pw.Value());
        if (!runner.HasValue()) {
            return core::Result<std::vector<std::uint32_t>>::Fail(runner.Err());
        }
        auto registry = std::move(runner).Value().EnsureRegistry();
        if (!registry.HasValue()) {
            return core::Result<std::vector<std::uint32_t>>::Fail(registry.Err());
        }
        auto up = std::move(runner).Value().Up(migrations_dir);
        if (!up.HasValue()) {
            // 任一分片迁移失败即中止（禁止「部分分片已升级」被当成成功）
            return core::Result<std::vector<std::uint32_t>>::Fail(up.Err());
        }
        applied.insert(applied.end(), up.Value().begin(), up.Value().end());
    }
    return core::Result<std::vector<std::uint32_t>>::Ok(std::move(applied));
}

}  // namespace mmo::data::mysql
