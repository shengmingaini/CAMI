// server/dataservice/src/mysql/repository.cpp
//
// TASK-028 §15.8 / §16 · 泛型仓储的非模板部分（列映射 + SQL 构造）。
//
// 标识符一律反引号包裹（`character` 是 MySQL 保留字，必须转义）且先过白名单校验；
// 值一律走 ? 占位符。

#include "mmo/data/mysql/repository.h"

#include <charconv>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mmo/data/mysql/sql_builder.h"

namespace mmo::data::mysql {

namespace {

core::Error Err(core::ErrorCode code, std::string_view msg) {
    return core::Error(code, msg, core::domain::kData);
}

/// 反引号包裹（调用前必须已通过 IsSafeIdentifier）。
std::string Q(std::string_view ident) {
    std::string out;
    out.reserve(ident.size() + 2);
    out.push_back('`');
    out += ident;
    out.push_back('`');
    return out;
}

core::Result<void> CheckSpec(const TableSpec& spec) {
    if (!IsSafeIdentifier(spec.table)) {
        return core::Result<void>::Fail(Err(core::ErrorCode::INVALID_ARGUMENT, "unsafe table name"));
    }
    if (!IsSafeIdentifier(spec.key_column)) {
        return core::Result<void>::Fail(Err(core::ErrorCode::INVALID_ARGUMENT, "unsafe key column"));
    }
    if (spec.is_multi_row() && !IsSafeIdentifier(spec.sub_key_column)) {
        return core::Result<void>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "unsafe sub key column"));
    }
    if (!IsSafeIdentifier(spec.version_column)) {
        return core::Result<void>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "unsafe version column"));
    }
    if (spec.columns.empty()) {
        return core::Result<void>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "table spec has no columns"));
    }
    bool has_key = false;
    bool has_version = false;
    for (const auto& c : spec.columns) {
        if (!IsSafeIdentifier(c)) {
            return core::Result<void>::Fail(
                Err(core::ErrorCode::INVALID_ARGUMENT, "unsafe column name"));
        }
        if (c == spec.key_column) has_key = true;
        if (c == spec.version_column) has_version = true;
    }
    if (!has_key || !has_version) {
        return core::Result<void>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "spec must contain key and version columns"));
    }
    return core::Result<void>::Ok();
}

std::string JoinQuoted(const std::vector<std::string>& cols, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (i != 0) out += sep;
        out += Q(cols[i]);
    }
    return out;
}

std::string Placeholders(std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) out += ',';
        out += '?';
    }
    return out;
}

}  // namespace

// ============================ 列名视图 ============================

RowView MakeRowView(const TableSpec& spec, const MySqlRow& row) {
    RowView view;
    if (row.values.size() != spec.columns.size()) return view;  // 空视图 = 列数不匹配
    for (std::size_t i = 0; i < spec.columns.size(); ++i) {
        const MySqlValue& v = row.values[i];
        if (v.is_null()) {
            view.emplace(spec.columns[i], std::nullopt);
        } else {
            view.emplace(spec.columns[i], std::optional<std::string>(v.str));
        }
    }
    return view;
}

std::optional<std::string> ViewText(const RowView& v, std::string_view column) {
    const auto it = v.find(std::string(column));
    if (it == v.end()) return std::nullopt;
    return it->second;
}

std::optional<std::int64_t> ViewInt(const RowView& v, std::string_view column) {
    const auto t = ViewText(v, column);
    if (!t.has_value() || t->empty()) return std::nullopt;
    std::int64_t out = 0;
    const char* first = t->data();
    const char* last = first + t->size();
    const auto r = std::from_chars(first, last, out);
    if (r.ec != std::errc{} || r.ptr != last) return std::nullopt;
    return out;
}

std::uint32_t ViewVersion(const RowView& v, const TableSpec& spec) {
    const auto n = ViewInt(v, spec.version_column);
    if (!n.has_value() || *n <= 0) return 0;
    return static_cast<std::uint32_t>(*n);
}

core::Result<MySqlParams> OrderParams(
    const TableSpec& spec, const std::unordered_map<std::string, MySqlValue>& by_column) {
    auto ok = CheckSpec(spec);
    if (!ok.HasValue()) return core::Result<MySqlParams>::Fail(ok.Err());

    MySqlParams out;
    out.reserve(spec.columns.size());
    for (const auto& c : spec.columns) {
        const auto it = by_column.find(c);
        if (it == by_column.end()) {
            return core::Result<MySqlParams>::Fail(
                Err(core::ErrorCode::INVALID_ARGUMENT, "row mapping misses a spec column"));
        }
        out.push_back(it->second);
    }
    return core::Result<MySqlParams>::Ok(std::move(out));
}

MySqlParams WithoutKeyColumn(const TableSpec& spec, const MySqlParams& params) {
    MySqlParams out;
    out.reserve(params.size());
    for (std::size_t i = 0; i < spec.columns.size() && i < params.size(); ++i) {
        if (spec.columns[i] == spec.key_column) continue;
        out.push_back(params[i]);
    }
    return out;
}

// ============================ SQL 构造 ============================

core::Result<std::string> BuildRepoSelectSql(const TableSpec& spec) {
    auto ok = CheckSpec(spec);
    if (!ok.HasValue()) return core::Result<std::string>::Fail(ok.Err());

    std::string sql = "SELECT ";
    sql += JoinQuoted(spec.columns, ",");
    sql += " FROM ";
    sql += Q(spec.table);
    sql += " WHERE ";
    sql += Q(spec.key_column);
    sql += "=?";
    if (spec.is_multi_row()) {
        sql += " ORDER BY ";
        sql += Q(spec.sub_key_column);
    }
    return core::Result<std::string>::Ok(std::move(sql));
}

core::Result<std::string> BuildRepoInsertSql(const TableSpec& spec) {
    auto ok = CheckSpec(spec);
    if (!ok.HasValue()) return core::Result<std::string>::Fail(ok.Err());

    std::string sql = "INSERT INTO ";
    sql += Q(spec.table);
    sql += " (";
    sql += JoinQuoted(spec.columns, ",");
    sql += ") VALUES (";
    sql += Placeholders(spec.columns.size());
    sql += ')';
    return core::Result<std::string>::Ok(std::move(sql));
}

core::Result<std::string> BuildRepoUpsertSql(const TableSpec& spec) {
    auto ok = CheckSpec(spec);
    if (!ok.HasValue()) return core::Result<std::string>::Fail(ok.Err());
    if (spec.is_multi_row()) {
        return core::Result<std::string>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "multi-row table must use insert path"));
    }

    std::string sql = "INSERT INTO ";
    sql += Q(spec.table);
    sql += " (";
    sql += JoinQuoted(spec.columns, ",");
    sql += ") VALUES (";
    sql += Placeholders(spec.columns.size());
    sql += ") ON DUPLICATE KEY UPDATE ";
    bool first = true;
    for (const auto& c : spec.columns) {
        if (c == spec.key_column) continue;
        if (!first) sql += ',';
        first = false;
        sql += Q(c);
        sql += "=VALUES(";
        sql += Q(c);
        sql += ')';
    }
    return core::Result<std::string>::Ok(std::move(sql));
}

core::Result<std::string> BuildRepoGuardedUpdateSql(const TableSpec& spec) {
    auto ok = CheckSpec(spec);
    if (!ok.HasValue()) return core::Result<std::string>::Fail(ok.Err());
    if (spec.is_multi_row()) {
        return core::Result<std::string>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "multi-row table has no single-row guard"));
    }

    std::string sql = "UPDATE ";
    sql += Q(spec.table);
    sql += " SET ";
    bool first = true;
    for (const auto& c : spec.columns) {
        if (c == spec.key_column) continue;
        if (!first) sql += ',';
        first = false;
        sql += Q(c);
        sql += "=?";
    }
    sql += " WHERE ";
    sql += Q(spec.key_column);
    sql += "=? AND ";
    sql += Q(spec.version_column);
    sql += "=?";
    return core::Result<std::string>::Ok(std::move(sql));
}

core::Result<std::string> BuildRepoDeleteByKeySql(const TableSpec& spec) {
    auto ok = CheckSpec(spec);
    if (!ok.HasValue()) return core::Result<std::string>::Fail(ok.Err());

    std::string sql = "DELETE FROM ";
    sql += Q(spec.table);
    sql += " WHERE ";
    sql += Q(spec.key_column);
    sql += "=?";
    return core::Result<std::string>::Ok(std::move(sql));
}

}  // namespace mmo::data::mysql
