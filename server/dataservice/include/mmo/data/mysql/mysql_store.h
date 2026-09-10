// server/dataservice/include/mmo/data/mysql/mysql_store.h
//
// TASK-028 · MySQL 权威存储适配器（§7 / §15.4 / §15.5）。
//
// 实现 TASK-026 冻结的 IDataStore（business-agnostic 键值 + 版本契约）：
//   键 `<domain>:<id>` -> 分片（ShardRouter）-> 该分片库内 `kv_store` 表的一行。
//   payload 是不透明字节（业务层负责编解码，§21 禁止把业务语义混进接口）；
//   version 列即乐观锁（§20.4）。
//
// 事务（§9 / §21）：
//   - BatchSave 内部**按分片分组**，每组一个单分片事务；绝不发起跨分片事务。
//   - BatchSaveAtomic 要求同分片，跨分片直接返回明确错误（禁止静默降级）。
//
// 线程模型（§9）：由 DataService 的 Persistence 线程池调用；禁止在 GameNode Tick 内调用。

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/data/health.h"
#include "mmo/data/idata_store.h"
#include "mmo/data/mysql/connection_pool.h"
#include "mmo/data/mysql/mysql_config.h"
#include "mmo/data/mysql/shard_router.h"

namespace mmo::data::mysql {

class MySqlStore final : public mmo::data::IDataStore {
public:
    /// 构造：校验分片配置一致性，并为每个分片建立连接池。
    /// 实例不可达时返回明确错误（§19：调用方仍可降级启动，不崩溃）。
    static core::Result<std::unique_ptr<MySqlStore>> Create(const ShardConfig& shard_cfg,
                                                            const MySqlConfig& cfg);

    /// 单分片便捷构造（测试/迁移工具用，等价于 shard_count=1）。
    static core::Result<std::unique_ptr<MySqlStore>> CreateSingle(const ShardEndpoint& ep,
                                                                  const MySqlConfig& cfg);

    ~MySqlStore() override;
    MySqlStore(const MySqlStore&) = delete;
    MySqlStore& operator=(const MySqlStore&) = delete;

    // ---- IDataStore（TASK-026 冻结契约）----
    core::Result<std::optional<mmo::data::Record>> Load(const mmo::data::DataKey& key) override;
    core::Result<void> Save(const mmo::data::Record& rec,
                            mmo::data::VersionCheck vc = {}) override;
    core::Result<void> Delete(const mmo::data::DataKey& key,
                              mmo::data::VersionCheck vc = {}) override;
    core::Result<std::vector<mmo::data::Record>> BatchLoad(
        std::span<const mmo::data::DataKey> keys) override;
    core::Result<std::vector<mmo::data::BatchOutcome>> BatchSave(
        std::span<const mmo::data::Record> recs) override;

    // ---- TASK-028 扩展 ----

    /// 对所有分片执行迁移（§15.6 / §20.6）。任一失败即中止并返回该错误（禁止半应用静默）。
    core::Result<std::vector<std::uint32_t>> Migrate(std::string_view migrations_dir);

    /// 原子批量写入：**要求全部同分片**；跨分片返回 INVALID_ARGUMENT（§20.5）。
    /// 单分片内走一个事务，任一条失败整体回滚。
    core::Result<void> BatchSaveAtomic(std::span<const mmo::data::Record> recs);

    /// 某分片健康状态（§7 签名）。
    HealthStatus Health(std::uint32_t shard) const noexcept;

    /// 某分片连接池指标。
    MySqlPoolStats pool_stats(std::uint32_t shard) const;

    /// 借出某分片连接（仓储层复用同一池，避免重复建连）。
    core::Result<MySqlLease> AcquireShard(std::uint32_t shard);

    /// 上报语句耗时（慢查询统计，§15.3）。
    void NoteStatement(std::uint32_t shard, std::string_view sql, std::uint64_t elapsed_ns);

    const ShardRouter& router() const noexcept { return router_; }
    const MySqlConfig& config() const noexcept { return cfg_; }

    /// 供仓储层构造 SQL 时使用的标识符白名单校验入口。
    static bool IsKnownTable(std::string_view table) noexcept;

private:
    MySqlStore(ShardRouter router, MySqlConfig cfg,
               std::vector<std::unique_ptr<MySqlConnectionPool>> pools);

    /// 依据路由选出分片连接池；越界返回 INTERNAL_ERROR（不崩溃）。
    core::Result<MySqlConnectionPool*> PoolOf(std::uint32_t shard) const;

    ShardRouter router_;
    MySqlConfig cfg_;
    std::vector<std::unique_ptr<MySqlConnectionPool>> pools_;
    mutable std::mutex mtx_;
};

/// 同分片校验（纯逻辑，§20.5 单测目标）：跨分片返回 INVALID_ARGUMENT。
core::Result<std::uint32_t> RequireSingleShard(const ShardRouter& router,
                                               std::span<const mmo::data::DataKey> keys);

}  // namespace mmo::data::mysql
