// server/dataservice/include/mmo/data/mysql/sql_builder.h
//
// TASK-028 · SQL 构造（§16「乐观锁 SQL 生成」单测目标）。
//
// 全部为**纯函数**：不依赖连接、不做 IO，故可在无 MySQL 环境下单测。
// 标识符（表名/列名）一律经白名单校验后才拼入 SQL —— 值永远走参数绑定，绝不拼接。

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/data/record.h"

namespace mmo::data::mysql {

/// SQL 标识符（表名/列名）合法性校验：字母开头，仅含字母数字下划线，长度 ≤ 64。
/// 用于阻止把外部输入拼进 SQL。
bool IsSafeIdentifier(std::string_view ident) noexcept;

/// 保存策略（由 VersionCheck 决定，§21 禁止「无 version 乐观锁的并发写」）。
enum class SaveMode {
    Insert,         // required 且 expected_version == 0：期望「不存在」，冲突即 1062
    UpdateGuarded,  // required 且 expected_version > 0：UPDATE ... WHERE version = ?
    Upsert,         // !required：不校验版本，直接覆盖
};

/// 依据 VersionCheck 决定写策略（纯函数）。
SaveMode DecideSaveMode(const mmo::data::VersionCheck& vc) noexcept;

/// 乐观锁 UPDATE：`UPDATE t SET payload=?,version=?,updated_at=? WHERE id=? AND version=?`
/// affected_rows == 0 即为 VERSION_CONFLICT（§20.4）。
core::Result<std::string> BuildGuardedUpdateSql(std::string_view table, std::string_view key_column);

/// 无条件覆盖：`INSERT ... ON DUPLICATE KEY UPDATE payload=VALUES(payload), version=VALUES(version), updated_at=VALUES(updated_at)`
core::Result<std::string> BuildUpsertSql(std::string_view table, std::string_view key_column);

/// 期望插入：`INSERT INTO t (key,payload,version,updated_at) VALUES (?,?,?,?)`（重复键 1062 -> VERSION_CONFLICT）
core::Result<std::string> BuildInsertSql(std::string_view table, std::string_view key_column);

/// 带版本校验的删除：`DELETE FROM t WHERE id=? AND version=?`
core::Result<std::string> BuildGuardedDeleteSql(std::string_view table, std::string_view key_column);

/// 无版本校验的删除：`DELETE FROM t WHERE id=?`
core::Result<std::string> BuildPlainDeleteSql(std::string_view table, std::string_view key_column);

/// 单键读取：`SELECT payload,version FROM t WHERE id=?`
core::Result<std::string> BuildSelectSql(std::string_view table, std::string_view key_column);

/// 批量读取：`SELECT id,payload,version FROM t WHERE id IN (?,?,...)`
core::Result<std::string> BuildBatchSelectSql(std::string_view table, std::string_view key_column,
                                              std::size_t key_count);

/// 通用表建表（kv_store：IDataStore 的业务无关落地，§4 MySQL 为持久化权威）。
core::Result<std::string> BuildCreateKvStoreSql();

/// 迁移记录表建表（§8：迁移工具把已执行版本记录到 schema_migrations）。
core::Result<std::string> BuildCreateSchemaMigrationsSql();

/// 记录迁移成功：`INSERT ... ON DUPLICATE KEY UPDATE success=1, error=NULL, applied_at=NOW()`
core::Result<std::string> BuildRecordMigrationSql();

/// 记录迁移失败（补偿：半应用状态下 status 可见，禁止静默）。
core::Result<std::string> BuildRecordMigrationFailureSql();

/// 查询已应用版本：`SELECT version,name,success,error,applied_at FROM schema_migrations ORDER BY version`
core::Result<std::string> BuildSelectMigrationsSql();

}  // namespace mmo::data::mysql
