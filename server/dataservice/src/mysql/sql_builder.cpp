// server/dataservice/src/mysql/sql_builder.cpp
//
// TASK-028 §16 · SQL 构造实现（纯函数，无 IO，故可离线单测）。
//
// 安全约定：标识符经 IsSafeIdentifier 校验后才拼接；**值一律走参数占位符 ?**。

#include "mmo/data/mysql/sql_builder.h"

#include <string>

#include "mmo/data/mysql/mysql_config.h"

namespace mmo::data::mysql {

namespace {

core::Error Err(core::ErrorCode code, const std::string& msg) {
    return core::Error(code, msg, core::domain::kData);
}

core::Result<std::string> Build(const std::string& sql) {
    return core::Result<std::string>::Ok(sql);
}

core::Result<void> RequireIdent(const std::string_view ident, const char* what) {
    if (!IsSafeIdentifier(ident)) {
        return core::Result<void>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, std::string("unsafe ") + what));
    }
    return core::Result<void>::Ok();
}

}  // namespace

bool IsSafeIdentifier(std::string_view ident) noexcept {
    if (ident.empty() || ident.size() > 64) return false;
    const unsigned char c0 = static_cast<unsigned char>(ident[0]);
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_')) return false;
    for (const char ch : ident) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

SaveMode DecideSaveMode(const mmo::data::VersionCheck& vc) noexcept {
    if (!vc.required) return SaveMode::Upsert;
    return vc.expected_version == 0 ? SaveMode::Insert : SaveMode::UpdateGuarded;
}

core::Result<std::string> BuildGuardedUpdateSql(std::string_view table, std::string_view key_column) {
    auto t = RequireIdent(table, "table");
    if (!t.HasValue()) return core::Result<std::string>::Fail(t.Err());
    auto k = RequireIdent(key_column, "key column");
    if (!k.HasValue()) return core::Result<std::string>::Fail(k.Err());

    std::string sql;
    sql.reserve(160);
    sql += "UPDATE ";
    sql += table;
    sql += " SET payload=?,version=?,updated_at=NOW() WHERE ";
    sql += key_column;
    sql += "=? AND version=?";
    return Build(sql);
}

core::Result<std::string> BuildUpsertSql(std::string_view table, std::string_view key_column) {
    auto t = RequireIdent(table, "table");
    if (!t.HasValue()) return core::Result<std::string>::Fail(t.Err());
    auto k = RequireIdent(key_column, "key column");
    if (!k.HasValue()) return core::Result<std::string>::Fail(k.Err());

    std::string sql;
    sql.reserve(240);
    sql += "INSERT INTO ";
    sql += table;
    sql += " (";
    sql += key_column;
    sql += ",payload,version,updated_at) VALUES (?,?,?,NOW()) ON DUPLICATE KEY UPDATE"
           " payload=VALUES(payload),version=VALUES(version),updated_at=VALUES(updated_at)";
    return Build(sql);
}

core::Result<std::string> BuildInsertSql(std::string_view table, std::string_view key_column) {
    auto t = RequireIdent(table, "table");
    if (!t.HasValue()) return core::Result<std::string>::Fail(t.Err());
    auto k = RequireIdent(key_column, "key column");
    if (!k.HasValue()) return core::Result<std::string>::Fail(k.Err());

    std::string sql;
    sql.reserve(160);
    sql += "INSERT INTO ";
    sql += table;
    sql += " (";
    sql += key_column;
    sql += ",payload,version,updated_at) VALUES (?,?,?,NOW())";
    return Build(sql);
}

core::Result<std::string> BuildGuardedDeleteSql(std::string_view table, std::string_view key_column) {
    auto t = RequireIdent(table, "table");
    if (!t.HasValue()) return core::Result<std::string>::Fail(t.Err());
    auto k = RequireIdent(key_column, "key column");
    if (!k.HasValue()) return core::Result<std::string>::Fail(k.Err());

    std::string sql;
    sql.reserve(120);
    sql += "DELETE FROM ";
    sql += table;
    sql += " WHERE ";
    sql += key_column;
    sql += "=? AND version=?";
    return Build(sql);
}

core::Result<std::string> BuildPlainDeleteSql(std::string_view table, std::string_view key_column) {
    auto t = RequireIdent(table, "table");
    if (!t.HasValue()) return core::Result<std::string>::Fail(t.Err());
    auto k = RequireIdent(key_column, "key column");
    if (!k.HasValue()) return core::Result<std::string>::Fail(k.Err());

    std::string sql;
    sql.reserve(120);
    sql += "DELETE FROM ";
    sql += table;
    sql += " WHERE ";
    sql += key_column;
    sql += "=?";
    return Build(sql);
}

core::Result<std::string> BuildSelectSql(std::string_view table, std::string_view key_column) {
    auto t = RequireIdent(table, "table");
    if (!t.HasValue()) return core::Result<std::string>::Fail(t.Err());
    auto k = RequireIdent(key_column, "key column");
    if (!k.HasValue()) return core::Result<std::string>::Fail(k.Err());

    std::string sql;
    sql.reserve(120);
    sql += "SELECT payload,version FROM ";
    sql += table;
    sql += " WHERE ";
    sql += key_column;
    sql += "=?";
    return Build(sql);
}

core::Result<std::string> BuildBatchSelectSql(std::string_view table, std::string_view key_column,
                                              std::size_t key_count) {
    auto t = RequireIdent(table, "table");
    if (!t.HasValue()) return core::Result<std::string>::Fail(t.Err());
    auto k = RequireIdent(key_column, "key column");
    if (!k.HasValue()) return core::Result<std::string>::Fail(k.Err());
    if (key_count == 0) {
        return core::Result<std::string>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "batch select requires key_count > 0"));
    }

    std::string sql;
    sql.reserve(64 + key_count * 3);
    sql += "SELECT ";
    sql += key_column;
    sql += ",payload,version FROM ";
    sql += table;
    sql += " WHERE ";
    sql += key_column;
    sql += " IN (";
    for (std::size_t i = 0; i < key_count; ++i) {
        if (i != 0) sql += ',';
        sql += '?';
    }
    sql += ')';
    return Build(sql);
}

core::Result<std::string> BuildCreateKvStoreSql() {
    // 单列主键 `k`，便于复用通用 INSERT/UPSERT/SELECT 构造器；业务键已自带 <domain>:<id>。
    // payload 为 LONGBLOB：业务层序列化后的**二进制安全**字节（§21 接口不含业务语义）。
    std::string sql;
    sql.reserve(300);
    sql += "CREATE TABLE IF NOT EXISTS ";
    sql += tables::kKvStore;
    sql += " (k VARCHAR(191) NOT NULL,"
           " payload LONGBLOB NOT NULL,"
           " version INT UNSIGNED NOT NULL DEFAULT 0,"
           " updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
           " PRIMARY KEY (k)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    return Build(sql);
}

core::Result<std::string> BuildCreateSchemaMigrationsSql() {
    std::string sql;
    sql.reserve(320);
    sql += "CREATE TABLE IF NOT EXISTS ";
    sql += tables::kSchemaMigrations;
    sql += " (version INT UNSIGNED NOT NULL,"
           " name VARCHAR(255) NOT NULL,"
           " success TINYINT NOT NULL DEFAULT 0,"
           " error TEXT NULL,"
           " applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
           " PRIMARY KEY (version)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    return Build(sql);
}

core::Result<std::string> BuildRecordMigrationSql() {
    std::string sql;
    sql.reserve(200);
    sql += "INSERT INTO ";
    sql += tables::kSchemaMigrations;
    sql += " (version,name,success,error) VALUES (?,?,1,NULL)"
           " ON DUPLICATE KEY UPDATE name=VALUES(name),success=1,error=NULL,"
           " applied_at=CURRENT_TIMESTAMP";
    return Build(sql);
}

core::Result<std::string> BuildRecordMigrationFailureSql() {
    std::string sql;
    sql.reserve(220);
    sql += "INSERT INTO ";
    sql += tables::kSchemaMigrations;
    sql += " (version,name,success,error) VALUES (?,?,0,?)"
           " ON DUPLICATE KEY UPDATE name=VALUES(name),success=0,error=VALUES(error),"
           " applied_at=CURRENT_TIMESTAMP";
    return Build(sql);
}

core::Result<std::string> BuildSelectMigrationsSql() {
    std::string sql;
    sql.reserve(140);
    sql += "SELECT version,name,success,error FROM ";
    sql += tables::kSchemaMigrations;
    sql += " ORDER BY version";
    return Build(sql);
}

}  // namespace mmo::data::mysql
