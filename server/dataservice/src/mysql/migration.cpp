// server/dataservice/src/mysql/migration.cpp
//
// TASK-028 §15.6 / §19 · Schema 迁移实现。
//
// 禁止半应用状态（§21）：MySQL 的 DDL 隐式提交、无法回滚，故采用
// 「逐版本执行 -> 成功写 success=1；失败写 success=0 + error 并立即中止」的补偿策略：
// 失败状态在 schema_migrations 中可查（status 可见），重跑不会静默跳过，也不会假装成功。
// 迁移 SQL 一律用 CREATE TABLE IF NOT EXISTS 等幂等写法，保证修复后可安全重跑。

#include "mmo/data/mysql/migration.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "mmo/core/config/config_manager.h"
#include "mmo/data/mysql/sql_builder.h"

namespace mmo::data::mysql {

namespace {

core::Error Err(core::ErrorCode code, std::string_view msg) {
    return core::Error(code, msg, core::domain::kData);
}

bool IsDigits(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (const char ch : s) {
        if (ch < '0' || ch > '9') return false;
    }
    return true;
}

/// 去掉行注释与块注释（保留字符串字面量内的内容不误删）。
std::string StripComments(std::string_view sql) {
    std::string out;
    out.reserve(sql.size());
    bool in_squote = false;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        if (in_squote) {
            out.push_back(c);
            if (c == '\\' && i + 1 < sql.size()) {
                out.push_back(sql[++i]);
            } else if (c == '\'') {
                in_squote = false;
            }
            continue;
        }
        if (c == '\'') {
            in_squote = true;
            out.push_back(c);
            continue;
        }
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') ++i;
            out.push_back('\n');
            continue;
        }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
            ++i;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool IsBlank(std::string_view s) noexcept {
    for (const char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
}

/// 去掉首尾空白（语句切分后去掉注释残留的换行/空格，保证切分结果稳定可断言）。
std::string Trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return std::string(s.substr(b, e - b));
}

}  // namespace

std::vector<std::string> MigrationRunner::SplitStatements(std::string_view sql) {
    const std::string cleaned = StripComments(sql);
    std::vector<std::string> out;
    std::string cur;
    bool in_squote = false;
    for (std::size_t i = 0; i < cleaned.size(); ++i) {
        const char c = cleaned[i];
        if (in_squote) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < cleaned.size()) {
                cur.push_back(cleaned[++i]);
            } else if (c == '\'') {
                in_squote = false;
            }
            continue;
        }
        if (c == '\'') {
            in_squote = true;
            cur.push_back(c);
            continue;
        }
        if (c == ';') {
            if (!IsBlank(cur)) out.push_back(Trim(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!IsBlank(cur)) out.push_back(Trim(cur));
    return out;
}

core::Result<std::uint32_t> MigrationRunner::ParseVersion(std::string_view filename) {
    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos || filename.substr(dot) != ".sql") {
        return core::Result<std::uint32_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "migration filename must end with .sql"));
    }
    const auto us = filename.find('_');
    if (us == std::string_view::npos || us == 0) {
        return core::Result<std::uint32_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "migration filename must be NNN_name.sql"));
    }
    const std::string_view digits = filename.substr(0, us);
    if (!IsDigits(digits)) {
        return core::Result<std::uint32_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "migration version prefix must be digits"));
    }
    std::uint32_t version = 0;
    const char* first = digits.data();
    const char* last = first + digits.size();
    const auto r = std::from_chars(first, last, version);
    if (r.ec != std::errc{} || r.ptr != last || version == 0) {
        return core::Result<std::uint32_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "migration version must be >= 1"));
    }
    return core::Result<std::uint32_t>::Ok(version);
}

core::Result<MigrationRunner> MigrationRunner::Create(const ShardEndpoint& ep, const MySqlConfig& cfg,
                                                     std::string_view password) {
    auto conn = MySqlConnection::Open(ep, cfg, password);
    if (!conn.HasValue()) {
        return core::Result<MigrationRunner>::Fail(conn.Err());
    }
    return core::Result<MigrationRunner>::Ok(MigrationRunner(std::move(conn).Value()));
}

core::Result<std::vector<MigrationFile>> MigrationRunner::Discover(std::string_view dir) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root{std::string(dir)};
    if (!fs::is_directory(root, ec) || ec) {
        return core::Result<std::vector<MigrationFile>>::Fail(
            Err(core::ErrorCode::INTERNAL_ERROR, "migrations dir not found"));
    }

    std::vector<MigrationFile> files;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        auto ver = ParseVersion(name);
        if (!ver.HasValue()) continue;  // 非规范命名跳过（README 等）
        auto content = mmo::core::ConfigManager::ReadFile(entry.path().string());
        if (!content.HasValue()) {
            return core::Result<std::vector<MigrationFile>>::Fail(content.Err());
        }
        MigrationFile f;
        f.version = ver.Value();
        f.name = name;
        f.path = entry.path().string();
        f.sql = std::move(content.Value());
        files.push_back(std::move(f));
    }

    std::sort(files.begin(), files.end(),
              [](const MigrationFile& a, const MigrationFile& b) { return a.version < b.version; });

    for (std::size_t i = 1; i < files.size(); ++i) {
        if (files[i].version == files[i - 1].version) {
            return core::Result<std::vector<MigrationFile>>::Fail(
                Err(core::ErrorCode::INVALID_ARGUMENT, "duplicate migration version"));
        }
    }
    return core::Result<std::vector<MigrationFile>>::Ok(std::move(files));
}

core::Result<void> MigrationRunner::EnsureRegistry() const {
    auto ddl = BuildCreateSchemaMigrationsSql();
    if (!ddl.HasValue()) return core::Result<void>::Fail(ddl.Err());
    auto rs = conn_.Execute(ddl.Value());
    if (!rs.HasValue()) return core::Result<void>::Fail(rs.Err());
    return core::Result<void>::Ok();
}

core::Result<bool> MigrationRunner::RegistryExists() const {
    const std::string sql =
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE()"
        " AND table_name = '" +
        std::string(tables::kSchemaMigrations) + "'";
    auto rs = conn_.Execute(sql);
    if (!rs.HasValue()) return core::Result<bool>::Fail(rs.Err());
    if (rs.Value().rows.empty()) return core::Result<bool>::Ok(false);
    const auto n = rs.Value().rows.front().Int(0);
    return core::Result<bool>::Ok(n.has_value() && *n > 0);
}

core::Result<std::vector<MigrationStatus>> MigrationRunner::Status(std::string_view dir) const {
    auto files = Discover(dir);
    if (!files.HasValue()) return core::Result<std::vector<MigrationStatus>>::Fail(files.Err());

    std::vector<MigrationStatus> out;
    out.reserve(files.Value().size());

    auto exists = RegistryExists();
    if (!exists.HasValue()) return core::Result<std::vector<MigrationStatus>>::Fail(exists.Err());

    std::vector<MigrationStatus> recorded;
    if (exists.Value()) {
        auto sql = BuildSelectMigrationsSql();
        if (!sql.HasValue()) return core::Result<std::vector<MigrationStatus>>::Fail(sql.Err());
        auto rs = conn_.Execute(sql.Value());
        if (!rs.HasValue()) return core::Result<std::vector<MigrationStatus>>::Fail(rs.Err());
        for (const auto& row : rs.Value().rows) {
            MigrationStatus st;
            const auto ver = row.Int(0);
            st.version = ver.has_value() && *ver > 0 ? static_cast<std::uint32_t>(*ver) : 0;
            const auto nm = row.Text(1);
            if (nm.has_value()) st.name = *nm;
            const auto ok = row.Int(2);
            st.applied = ok.has_value() && *ok == 1;
            st.failed = ok.has_value() && *ok == 0;
            const auto err = row.Text(3);
            if (err.has_value()) st.error = *err;
            recorded.push_back(std::move(st));
        }
    }

    for (const auto& f : files.Value()) {
        MigrationStatus st;
        st.version = f.version;
        st.name = f.name;
        for (const auto& r : recorded) {
            if (r.version == f.version) {
                st.applied = r.applied;
                st.failed = r.failed;
                st.error = r.error;
                break;
            }
        }
        out.push_back(std::move(st));
    }
    return core::Result<std::vector<MigrationStatus>>::Ok(std::move(out));
}

core::Result<std::string> MigrationRunner::DryRun(std::string_view dir) const {
    auto status = Status(dir);
    if (!status.HasValue()) return core::Result<std::string>::Fail(status.Err());
    auto files = Discover(dir);
    if (!files.HasValue()) return core::Result<std::string>::Fail(files.Err());

    std::string out = "dry-run（不写库）:\n";
    std::size_t pending = 0;
    for (const auto& st : status.Value()) {
        const char* state = st.applied ? "applied" : (st.failed ? "FAILED" : "pending");
        out += "  [" + std::string(state) + "] ";
        out += st.name;
        if (st.failed) out += "  error=" + st.error;
        out += '\n';
        if (!st.applied) ++pending;
    }
    for (const auto& f : files.Value()) {
        const auto stmts = SplitStatements(f.sql);
        out += "  would run " + std::to_string(stmts.size()) + " statement(s) from " + f.name + '\n';
    }
    out += "pending_versions=" + std::to_string(pending) + '\n';
    return core::Result<std::string>::Ok(std::move(out));
}

core::Result<std::vector<std::uint32_t>> MigrationRunner::Up(std::string_view dir) const {
    auto registry = EnsureRegistry();
    if (!registry.HasValue()) {
        return core::Result<std::vector<std::uint32_t>>::Fail(registry.Err());
    }
    auto status = Status(dir);
    if (!status.HasValue()) return core::Result<std::vector<std::uint32_t>>::Fail(status.Err());
    auto files = Discover(dir);
    if (!files.HasValue()) return core::Result<std::vector<std::uint32_t>>::Fail(files.Err());

    auto rec_sql = BuildRecordMigrationSql();
    if (!rec_sql.HasValue()) return core::Result<std::vector<std::uint32_t>>::Fail(rec_sql.Err());
    auto fail_sql = BuildRecordMigrationFailureSql();
    if (!fail_sql.HasValue()) return core::Result<std::vector<std::uint32_t>>::Fail(fail_sql.Err());

    std::vector<std::uint32_t> applied;
    for (const auto& f : files.Value()) {
        bool already = false;
        for (const auto& st : status.Value()) {
            if (st.version == f.version && st.applied) {
                already = true;
                break;
            }
        }
        if (already) continue;

        const auto stmts = SplitStatements(f.sql);
        for (const auto& s : stmts) {
            auto rs = conn_.Execute(s);
            if (!rs.HasValue()) {
                // 补偿记录：失败可见、可查，然后立即中止（禁止半应用静默）
                (void)conn_.ExecuteParams(fail_sql.Value(),
                                          MySqlParams{MySqlValue::Uint(f.version),
                                                      MySqlValue::Text(f.name),
                                                      MySqlValue::Text(std::string(rs.Err().Message()))});
                return core::Result<std::vector<std::uint32_t>>::Fail(rs.Err());
            }
        }
        auto rec = conn_.ExecuteParams(
            rec_sql.Value(), MySqlParams{MySqlValue::Uint(f.version), MySqlValue::Text(f.name)});
        if (!rec.HasValue()) {
            return core::Result<std::vector<std::uint32_t>>::Fail(rec.Err());
        }
        applied.push_back(f.version);
    }
    return core::Result<std::vector<std::uint32_t>>::Ok(std::move(applied));
}

}  // namespace mmo::data::mysql
