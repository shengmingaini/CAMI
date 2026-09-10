// server/dataservice/include/mmo/data/mysql/repository.h
//
// TASK-028 · 泛型仓储（§15.8）。
//
// 建立在 MySqlStore 之上（共用 ShardRouter + 每分片连接池），实现 TASK-026 的
// IRepository<T>。领域对象 <-> 行集合的编解码由 `EntityCodec<T>` 特化提供，
// 新增表**不需要修改本文件**（§27.4 扩展点）。
//
// 表形态（§8）：
//   - 单行表（account/character/guild/mail/equipment）：PK = key_column
//   - 多行表（inventory/quest）：PK = (key_column, sub_key_column)，GetById 返回聚合对象，
//     Put 在单分片事务内「删除旧子行 + 插入新子行」，失败整体回滚（§9 事务只在单分片内）。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/data/irepository.h"
#include "mmo/data/mysql/mysql_client.h"
#include "mmo/data/mysql/mysql_store.h"
#include "mmo/data/mysql/sql_builder.h"  // DecideSaveMode / SaveMode（模板实例化点需要）

namespace mmo::data::mysql {

/// 逻辑表映射描述。`columns` 的顺序即参数顺序与投影顺序。
struct TableSpec {
    std::string table;
    std::string key_column;            // 分片键（同时也是主键首列）
    std::string sub_key_column;        // 空 => 单行表
    std::vector<std::string> columns;  // 必须包含 key_column 与 version_column
    std::string version_column{"version"};

    bool is_multi_row() const noexcept { return !sub_key_column.empty(); }
};

// ---- 结果行 -> 列名视图（脱离数据库即可单测「字段映射」，§16）----

using RowView = std::unordered_map<std::string, std::optional<std::string>>;

/// 按 spec.columns 把一行结果映射成 `列名 -> 文本`（列数不匹配返回空视图）。
RowView MakeRowView(const TableSpec& spec, const MySqlRow& row);

std::optional<std::string> ViewText(const RowView& v, std::string_view column);
std::optional<std::int64_t> ViewInt(const RowView& v, std::string_view column);
std::uint32_t ViewVersion(const RowView& v, const TableSpec& spec);

/// 按 spec.columns 顺序组装参数；缺列返回 INVALID_ARGUMENT（禁止静默补 NULL）。
core::Result<MySqlParams> OrderParams(
    const TableSpec& spec, const std::unordered_map<std::string, MySqlValue>& by_column);

/// 去掉 key 列后的参数（guarded UPDATE 的 SET 列表不含 key 列，参数必须同步裁掉）。
MySqlParams WithoutKeyColumn(const TableSpec& spec, const MySqlParams& params);

// ---- SQL 构造（纯函数，§16 单测目标）----

core::Result<std::string> BuildRepoSelectSql(const TableSpec& spec);
core::Result<std::string> BuildRepoInsertSql(const TableSpec& spec);
core::Result<std::string> BuildRepoUpsertSql(const TableSpec& spec);
core::Result<std::string> BuildRepoGuardedUpdateSql(const TableSpec& spec);
core::Result<std::string> BuildRepoDeleteByKeySql(const TableSpec& spec);

// ---- 编解码扩展点（§27.4）----

/// 领域对象 <-> 行集合。
///   - KeyOf：取分片键（仓储不感知业务字段，由编解码提供）
///   - ToRows：单行表返回 1 组参数、多行表返回 N 组；顺序与 spec.columns 一致
///   - FromRows：把 SELECT 结果还原成领域对象（多行表聚合子行）
template <typename T>
struct EntityCodec {
    static core::Result<std::uint64_t> KeyOf(const T& entity);
    static core::Result<std::vector<MySqlParams>> ToRows(const T& entity, const TableSpec& spec);
    static core::Result<T> FromRows(const std::vector<MySqlRow>& rows, const TableSpec& spec);
};

/// 泛型仓储（实现 TASK-026 的 IRepository<T>）。
template <typename T>
class MySqlRepository final : public mmo::data::IRepository<T> {
public:
    MySqlRepository(MySqlStore& store, TableSpec spec)
        : store_(store), spec_(std::move(spec)) {}

    core::Result<std::optional<T>> GetById(std::uint64_t id) override;
    core::Result<void> Put(const T& entity, mmo::data::VersionCheck vc = {}) override;
    core::Result<void> Remove(std::uint64_t id) override;

    const TableSpec& spec() const noexcept { return spec_; }

private:
    core::Result<std::vector<MySqlRow>> SelectRows(std::uint64_t id);

    MySqlStore& store_;
    TableSpec spec_;
};

// ============================ 模板实现 ============================

template <typename T>
core::Result<std::vector<MySqlRow>> MySqlRepository<T>::SelectRows(std::uint64_t id) {
    auto sql = BuildRepoSelectSql(spec_);
    if (!sql.HasValue()) return core::Result<std::vector<MySqlRow>>::Fail(sql.Err());
    auto lease = store_.AcquireShard(store_.router().ShardOf(id));
    if (!lease.HasValue()) return core::Result<std::vector<MySqlRow>>::Fail(lease.Err());
    auto rs = lease.Value().handle()->ExecuteParams(sql.Value(), MySqlParams{MySqlValue::Uint(id)});
    if (!rs.HasValue()) return core::Result<std::vector<MySqlRow>>::Fail(rs.Err());
    return core::Result<std::vector<MySqlRow>>::Ok(std::move(rs.Value().rows));
}

template <typename T>
core::Result<std::optional<T>> MySqlRepository<T>::GetById(std::uint64_t id) {
    auto rows = SelectRows(id);
    if (!rows.HasValue()) return core::Result<std::optional<T>>::Fail(rows.Err());
    if (rows.Value().empty())
        return core::Result<std::optional<T>>::Ok(std::optional<T>(std::nullopt));
    auto decoded = EntityCodec<T>::FromRows(rows.Value(), spec_);
    if (!decoded.HasValue()) return core::Result<std::optional<T>>::Fail(decoded.Err());
    return core::Result<std::optional<T>>::Ok(std::optional<T>(std::move(decoded.Value())));
}

template <typename T>
core::Result<void> MySqlRepository<T>::Put(const T& entity, mmo::data::VersionCheck vc) {
    auto key = EntityCodec<T>::KeyOf(entity);
    if (!key.HasValue()) return core::Result<void>::Fail(key.Err());

    auto rows = EntityCodec<T>::ToRows(entity, spec_);
    if (!rows.HasValue()) return core::Result<void>::Fail(rows.Err());
    if (rows.Value().empty()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "repository: empty row set", core::domain::kData));
    }

    auto lease = store_.AcquireShard(store_.router().ShardOf(key.Value()));
    if (!lease.HasValue()) return core::Result<void>::Fail(lease.Err());
    MySqlConnection* conn = lease.Value().handle();

    if (!spec_.is_multi_row()) {
        if (DecideSaveMode(vc) == SaveMode::UpdateGuarded) {
            auto sql = BuildRepoGuardedUpdateSql(spec_);
            if (!sql.HasValue()) return core::Result<void>::Fail(sql.Err());
            // 参数顺序：SET 列（去掉 key 列，与 SQL 同步）-> WHERE key=? -> AND version=?
            MySqlParams params = WithoutKeyColumn(spec_, rows.Value().front());
            params.push_back(MySqlValue::Uint(key.Value()));
            params.push_back(MySqlValue::Uint(vc.expected_version));
            auto rs = conn->ExecuteParams(sql.Value(), params);
            if (!rs.HasValue()) return core::Result<void>::Fail(rs.Err());
            if (rs.Value().affected_rows == 0) {
                return core::Result<void>::Fail(core::Error(
                    core::ErrorCode::VERSION_CONFLICT, "repository: version mismatch",
                    core::domain::kData));
            }
            return core::Result<void>::Ok();
        }
        auto sql = BuildRepoUpsertSql(spec_);
        if (!sql.HasValue()) return core::Result<void>::Fail(sql.Err());
        auto rs = conn->ExecuteParams(sql.Value(), rows.Value().front());
        if (!rs.HasValue()) return core::Result<void>::Fail(rs.Err());
        return core::Result<void>::Ok();
    }

    // 多行表：单分片事务内整体替换（中途失败整体回滚）
    auto begin = conn->Begin();
    if (!begin.HasValue()) return begin;
    auto del_sql = BuildRepoDeleteByKeySql(spec_);
    if (!del_sql.HasValue()) {
        (void)conn->Rollback();
        return core::Result<void>::Fail(del_sql.Err());
    }
    auto del = conn->ExecuteParams(del_sql.Value(), MySqlParams{MySqlValue::Uint(key.Value())});
    if (!del.HasValue()) {
        (void)conn->Rollback();
        return core::Result<void>::Fail(del.Err());
    }
    auto ins_sql = BuildRepoInsertSql(spec_);
    if (!ins_sql.HasValue()) {
        (void)conn->Rollback();
        return core::Result<void>::Fail(ins_sql.Err());
    }
    for (const auto& p : rows.Value()) {
        auto ins = conn->ExecuteParams(ins_sql.Value(), p);
        if (!ins.HasValue()) {
            (void)conn->Rollback();
            return core::Result<void>::Fail(ins.Err());
        }
    }
    return conn->Commit();
}

template <typename T>
core::Result<void> MySqlRepository<T>::Remove(std::uint64_t id) {
    auto lease = store_.AcquireShard(store_.router().ShardOf(id));
    if (!lease.HasValue()) return core::Result<void>::Fail(lease.Err());
    auto sql = BuildRepoDeleteByKeySql(spec_);
    if (!sql.HasValue()) return core::Result<void>::Fail(sql.Err());
    // 单行表与多行表同一条 SQL：DELETE FROM t WHERE key=?（多行表即删掉全部子行）
    auto rs = lease.Value().handle()->ExecuteParams(sql.Value(), MySqlParams{MySqlValue::Uint(id)});
    if (!rs.HasValue()) return core::Result<void>::Fail(rs.Err());
    return core::Result<void>::Ok();
}

}  // namespace mmo::data::mysql
