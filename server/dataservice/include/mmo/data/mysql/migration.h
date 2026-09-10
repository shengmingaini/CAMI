// server/dataservice/include/mmo/data/mysql/migration.h
//
// TASK-028 · Schema 迁移（§8 / §15.6 / §19）。
//
// 约束：Schema 迁移只能由本任务提供的迁移工具执行，禁止手工改表（§4）。
// 已执行版本记录在 schema_migrations（§8）；禁止留下半应用状态（§21）：
//   - DDL 在 MySQL 中隐式提交、无法回滚，故采用「失败即补偿记录 + 中止」策略：
//     成功 -> success=1；失败 -> success=0 + error 文本，状态可查、重跑明确。

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/data/mysql/mysql_client.h"
#include "mmo/data/mysql/mysql_config.h"

namespace mmo::data::mysql {

/// 一个迁移文件（NNN_xxx.sql）。
struct MigrationFile {
    std::uint32_t version{0};  // 从文件名前缀解析（001 -> 1）
    std::string name;          // 文件名（含扩展名）
    std::string path;          // 完整路径
    std::string sql;           // 文件内容
};

/// 迁移状态（up / status 共用）。
struct MigrationStatus {
    std::uint32_t version{0};
    std::string name;
    bool applied{false};   // 已成功执行
    bool failed{false};    // 曾失败（补偿记录可见，禁止静默）
    std::string error;     // 失败原因
};

/// 迁移执行器（单分片；对多分片由 MySqlStore::Migrate 逐个调用）。
class MigrationRunner {
public:
    /// 建立迁移专用连接（不经过业务连接池，避免长事务占池）。
    static core::Result<MigrationRunner> Create(const ShardEndpoint& ep, const MySqlConfig& cfg,
                                               std::string_view password);

    /// 从文件名解析版本号：`001_init.sql` -> 1；非规范命名 -> INVALID_ARGUMENT。
    static core::Result<std::uint32_t> ParseVersion(std::string_view filename);

    /// 扫描目录下 `NNN_*.sql`，按版本升序返回（文件缺失返回 INTERNAL_ERROR，不伪造空集）。
    core::Result<std::vector<MigrationFile>> Discover(std::string_view dir) const;

    /// 确保 schema_migrations 存在（幂等）。
    core::Result<void> EnsureRegistry() const;

    /// 只读探测 schema_migrations 是否存在（dry-run 不得写库，§15.6）。
    core::Result<bool> RegistryExists() const;

    /// 查询已应用/失败状态。
    core::Result<std::vector<MigrationStatus>> Status(std::string_view dir) const;

    /// 干跑：返回将执行的迁移清单文本，**不写库**（§15.6 dry-run）。
    core::Result<std::string> DryRun(std::string_view dir) const;

    /// 执行尚未成功应用的迁移（按版本升序）；任一失败即中止并记录补偿，返回该错误。
    core::Result<std::vector<std::uint32_t>> Up(std::string_view dir) const;

    /// 把一个 SQL 文本切分为独立语句（去掉注释与空语句；供逐条执行与计数）。
    static std::vector<std::string> SplitStatements(std::string_view sql);

private:
    explicit MigrationRunner(MySqlConnection conn) : conn_(std::move(conn)) {}

    // 连接是迁移执行的载体而非 runner 的可观测状态：方法可 const（连接内部有 mutex 之外
    // 的会话状态，故用 mutable 承载），避免调用方被迫以非 const 引用持有 runner。
    mutable MySqlConnection conn_;
};

}  // namespace mmo::data::mysql
