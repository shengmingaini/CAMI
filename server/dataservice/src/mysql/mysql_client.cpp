// server/dataservice/src/mysql/mysql_client.cpp
//
// TASK-028 §15.1 / §15.3 · MySQL 连接与语句执行实现。
//
// 第三方头（MariaDB Connector/C 的 mysql.h）只在本文件可见：公开头用不透明 void* 持有句柄。
// 写路径一律走 prepared statement（参数绑定），值绝不拼进 SQL。

#include "mmo/data/mysql/mysql_client.h"

#include <mysql.h>

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "mmo/data/mysql/sql_builder.h"

namespace mmo::data::mysql {

namespace {

core::Error Err(core::ErrorCode code, std::string_view msg) {
    return core::Error(code, msg, core::domain::kData);
}

/// 秒级超时（MySQL C API 只接受整秒）；<1s 一律向上取 1s，避免 0 被解释为「无限等待」。
unsigned int TimeoutSeconds(core::DurationMs d) noexcept {
    const auto ms = d.count();
    if (ms <= 0) return 1;
    return static_cast<unsigned int>((ms + 999) / 1000);
}

/// 结果集 RAII（MYSQL_RES*）。
struct ResultGuard {
    MYSQL_RES* res{nullptr};
    ~ResultGuard() {
        if (res != nullptr) mysql_free_result(res);
    }
};

/// 每连接预编译语句缓存上限（本模块 SQL 形态只有十余条，上限只为防御异常增长）。
constexpr std::size_t kStmtCacheMax = 32;

/// 事务控制语句无结果集，统一折叠成 Result<void>。
core::Result<void> ToVoid(core::Result<MySqlResultSet>&& r) {
    if (!r.HasValue()) return core::Result<void>::Fail(r.Err());
    return core::Result<void>::Ok();
}

}  // namespace

// ============================ MySqlValue / MySqlRow ============================

MySqlValue MySqlValue::Null() { return MySqlValue{}; }

MySqlValue MySqlValue::Int(std::int64_t v) {
    MySqlValue x;
    x.type = MySqlValueType::Int64;
    x.i64 = v;
    return x;
}

MySqlValue MySqlValue::Uint(std::uint64_t v) {
    MySqlValue x;
    x.type = MySqlValueType::Uint64;
    std::memcpy(&x.i64, &v, sizeof(v));  // 位保持；绑定层按 unsigned 解释
    return x;
}

MySqlValue MySqlValue::Real(double v) {
    MySqlValue x;
    x.type = MySqlValueType::Double;
    x.dbl = v;
    return x;
}

MySqlValue MySqlValue::Text(std::string v) {
    MySqlValue x;
    x.type = MySqlValueType::Text;
    x.str = std::move(v);
    return x;
}

MySqlValue MySqlValue::Blob(std::string v) {
    MySqlValue x;
    x.type = MySqlValueType::Binary;
    x.str = std::move(v);
    return x;
}

std::optional<std::string> MySqlRow::Text(std::size_t i) const {
    if (i >= values.size() || values[i].is_null()) return std::nullopt;
    return values[i].str;
}

std::optional<std::int64_t> MySqlRow::Int(std::size_t i) const {
    const auto t = Text(i);
    if (!t.has_value() || t->empty()) return std::nullopt;
    std::int64_t v = 0;
    const char* first = t->data();
    const char* last = first + t->size();
    const auto r = std::from_chars(first, last, v);
    if (r.ec != std::errc{} || r.ptr != last) return std::nullopt;
    return v;
}

// ============================ 错误码判定 ============================

bool IsDeadlockCode(unsigned int c) noexcept {
    return c == 1213u /*ER_LOCK_DEADLOCK*/ || c == 1205u /*ER_LOCK_WAIT_TIMEOUT*/;
}

bool IsConnectionLostCode(unsigned int c) noexcept {
    switch (c) {
        case 2002u:  // CR_CONNECTION_ERROR（连接被拒/不可达）
        case 2003u:  // CR_CONN_HOST_ERROR
        case 2006u:  // CR_SERVER_GONE_ERROR
        case 2055u:  // CR_SERVER_LOST
            return true;
        default:
            return false;
    }
}

core::ErrorCode MapMySqlError(unsigned int code) noexcept {
    if (IsDeadlockCode(code)) return core::ErrorCode::BUSY;        // 可有限重试（§19）
    if (IsConnectionLostCode(code)) return core::ErrorCode::BUSY;  // 可重连（§17/§19）
    if (code == 2013u) return core::ErrorCode::TIMEOUT;            // 读超时/查询期连接中断
    switch (code) {
        case 1062u:  // ER_DUP_ENTRY：期望插入但已存在 -> 版本冲突语义
        case 1022u:  // ER_DUP_KEY
            return core::ErrorCode::VERSION_CONFLICT;
        case 1044u:  // ER_DBACCESS_DENIED_ERROR
        case 1045u:  // ER_ACCESS_DENIED_ERROR
            return core::ErrorCode::UNAUTHORIZED;
        default:
            return core::ErrorCode::INTERNAL_ERROR;
    }
}

core::Result<std::string> ResolveMySqlPassword(const MySqlConfig& cfg) {
    if (cfg.password_env.empty()) {
        return core::Result<std::string>::Ok(std::string{});
    }
    const char* p = std::getenv(cfg.password_env.c_str());
    if (p == nullptr) {
        // §19：env 名给了但变量未设置 -> 明确失败，禁止用空密码兜底假装成功。
        return core::Result<std::string>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "mysql password env not set"));
    }
    return core::Result<std::string>::Ok(std::string(p));
}

// ============================ MySqlConnection ============================

core::Error MySqlConnection::RecordError(unsigned int code, std::string_view msg) {
    err_.code = code;
    err_.message.assign(msg);
    // 连接类错误 / 读写超时 -> 标记损坏：池归还时丢弃重建，
    // 否则超时后 socket 内残留的未读响应会造成下次复用的协议错位。
    if (IsConnectionLostCode(code) || code == 2013u) broken_ = true;
    return core::Error(MapMySqlError(code), err_.message, core::domain::kData);
}

MySqlConnection::~MySqlConnection() { Close(); }

MySqlConnection::MySqlConnection(MySqlConnection&& o) noexcept
    : raw_(o.raw_), broken_(o.broken_), ep_(std::move(o.ep_)), err_(std::move(o.err_)),
      server_version_(std::move(o.server_version_)), stmt_cache_(std::move(o.stmt_cache_)) {
    o.raw_ = nullptr;  // 句柄归属已转移，源对象不得再关闭语句/连接
    o.stmt_cache_.clear();
}

MySqlConnection& MySqlConnection::operator=(MySqlConnection&& o) noexcept {
    if (this != &o) {
        Close();
        raw_ = o.raw_;
        broken_ = o.broken_;
        ep_ = std::move(o.ep_);
        err_ = std::move(o.err_);
        server_version_ = std::move(o.server_version_);
        stmt_cache_ = std::move(o.stmt_cache_);
        o.raw_ = nullptr;
        o.stmt_cache_.clear();
    }
    return *this;
}

void MySqlConnection::Close() noexcept {
    ClearStmtCache();  // 必须先于 mysql_close：语句句柄依赖连接
    if (raw_ != nullptr) {
        mysql_close(static_cast<MYSQL*>(raw_));
        raw_ = nullptr;
    }
}

void MySqlConnection::ClearStmtCache() noexcept {
    for (auto& kv : stmt_cache_) {
        if (kv.second != nullptr) mysql_stmt_close(static_cast<MYSQL_STMT*>(kv.second));
    }
    stmt_cache_.clear();
}

void MySqlConnection::DropStmt(std::string_view sql) noexcept {
    for (auto it = stmt_cache_.begin(); it != stmt_cache_.end(); ++it) {
        if (std::string_view(it->first) == sql) {
            if (it->second != nullptr) mysql_stmt_close(static_cast<MYSQL_STMT*>(it->second));
            stmt_cache_.erase(it);
            return;
        }
    }
}

void* MySqlConnection::AcquireStmt(void* raw_v, std::string_view sql) {
    const std::string key(sql);
    for (const auto& kv : stmt_cache_) {
        if (kv.first == key) return kv.second;  // 命中：省掉 init + prepare 两次往返
    }

    MYSQL* raw = static_cast<MYSQL*>(raw_v);
    MYSQL_STMT* st = mysql_stmt_init(raw);
    if (st == nullptr) {
        (void)RecordError(0, "mysql_stmt_init failed");
        return nullptr;
    }
    if (mysql_stmt_prepare(st, key.data(), static_cast<unsigned long>(key.size())) != 0) {
        const unsigned int code = mysql_stmt_errno(st);
        const std::string msg = mysql_stmt_error(st);
        mysql_stmt_close(st);
        (void)RecordError(code, msg);
        return nullptr;
    }
    if (stmt_cache_.size() >= kStmtCacheMax) {
        // FIFO 淘汰：长连接上 SQL 形态有限（本模块只有十余条），上限只为防御异常增长
        if (stmt_cache_.front().second != nullptr) {
            mysql_stmt_close(static_cast<MYSQL_STMT*>(stmt_cache_.front().second));
        }
        stmt_cache_.erase(stmt_cache_.begin());
    }
    stmt_cache_.emplace_back(key, st);
    return st;
}

core::Result<MySqlConnection> MySqlConnection::Open(const ShardEndpoint& ep, const MySqlConfig& cfg,
                                                    std::string_view password) {
    MYSQL* raw = mysql_init(nullptr);
    if (raw == nullptr) {
        return core::Result<MySqlConnection>::Fail(
            Err(core::ErrorCode::INTERNAL_ERROR, "mysql_init failed"));
    }

    const unsigned int connect_s = TimeoutSeconds(cfg.connect_timeout);
    const unsigned int read_s = TimeoutSeconds(cfg.query_timeout);
    mysql_options(raw, MYSQL_OPT_CONNECT_TIMEOUT, &connect_s);
    mysql_options(raw, MYSQL_OPT_READ_TIMEOUT, &read_s);  // §19：查询超时落到 socket
    mysql_options(raw, MYSQL_OPT_WRITE_TIMEOUT, &read_s);
    mysql_options(raw, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    const std::string pw(password);
    // CLIENT_FOUND_ROWS：让 UPDATE 的 affected_rows 语义为「匹配行数」而非「实际变更行数」。
    // 乐观锁判定依赖它（§20.4）：否则「匹配到但值未变」的 UPDATE 会返回 0，
    // 被误判成 VERSION_CONFLICT。
    if (mysql_real_connect(raw, ep.host.c_str(), ep.user.c_str(), pw.c_str(), ep.database.c_str(),
                           static_cast<unsigned int>(ep.port), nullptr, CLIENT_FOUND_ROWS) == nullptr) {
        const unsigned int code = mysql_errno(raw);
        const std::string msg = mysql_error(raw);
        mysql_close(raw);
        return core::Result<MySqlConnection>::Fail(
            core::Error(MapMySqlError(code), msg, core::domain::kData));
    }

    MySqlConnection conn;
    conn.raw_ = raw;
    conn.ep_ = ep;
    const char* ver = mysql_get_server_info(raw);
    conn.server_version_ = (ver != nullptr) ? ver : "";
    return core::Result<MySqlConnection>::Ok(std::move(conn));
}

bool MySqlConnection::alive() noexcept {
    if (raw_ == nullptr) return false;
    MYSQL* raw = static_cast<MYSQL*>(raw_);
    if (mysql_ping(raw) == 0) return true;
    (void)RecordError(mysql_errno(raw), mysql_error(raw));
    ClearStmtCache();  // 连接已断：服务端语句句柄随之失效，禁止复用
    return false;
}

core::Result<MySqlResultSet> MySqlConnection::Execute(std::string_view sql) {
    if (raw_ == nullptr) {
        return core::Result<MySqlResultSet>::Fail(
            Err(core::ErrorCode::BUSY, "mysql connection is not open"));
    }
    MYSQL* raw = static_cast<MYSQL*>(raw_);

    if (mysql_real_query(raw, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        return core::Result<MySqlResultSet>::Fail(RecordError(mysql_errno(raw), mysql_error(raw)));
    }

    MySqlResultSet out;
    ResultGuard guard;
    guard.res = mysql_store_result(raw);
    if (guard.res == nullptr) {
        if (mysql_errno(raw) != 0) {
            return core::Result<MySqlResultSet>::Fail(
                RecordError(mysql_errno(raw), mysql_error(raw)));
        }
        out.affected_rows = static_cast<std::uint64_t>(mysql_affected_rows(raw));
        out.insert_id = static_cast<std::uint64_t>(mysql_insert_id(raw));
        return core::Result<MySqlResultSet>::Ok(std::move(out));
    }

    out.has_result_set = true;
    const unsigned int ncols = mysql_num_fields(guard.res);
    MYSQL_FIELD* fields = mysql_fetch_fields(guard.res);
    out.columns.resize(ncols);
    for (unsigned int i = 0; i < ncols; ++i) {
        out.columns[i].name = (fields[i].name != nullptr) ? fields[i].name : "";
    }

    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(guard.res)) != nullptr) {
        unsigned long* lens = mysql_fetch_lengths(guard.res);
        MySqlRow r;
        r.values.reserve(ncols);
        for (unsigned int i = 0; i < ncols; ++i) {
            if (row[i] == nullptr) {
                r.values.push_back(MySqlValue::Null());
            } else {
                const unsigned long len =
                    (lens != nullptr) ? lens[i] : static_cast<unsigned long>(std::strlen(row[i]));
                r.values.push_back(MySqlValue::Text(std::string(row[i], len)));
            }
        }
        out.rows.push_back(std::move(r));
    }
    if (mysql_errno(raw) != 0) {
        return core::Result<MySqlResultSet>::Fail(RecordError(mysql_errno(raw), mysql_error(raw)));
    }
    out.affected_rows = static_cast<std::uint64_t>(mysql_affected_rows(raw));
    return core::Result<MySqlResultSet>::Ok(std::move(out));
}

core::Result<MySqlResultSet> MySqlConnection::ExecuteParams(std::string_view sql,
                                                            const MySqlParams& params) {
    if (raw_ == nullptr) {
        return core::Result<MySqlResultSet>::Fail(
            Err(core::ErrorCode::BUSY, "mysql connection is not open"));
    }
    MYSQL* raw = static_cast<MYSQL*>(raw_);

    // 语句句柄来自每连接缓存：命中即复用（省掉 init + prepare 各一次往返，§22）。
    MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(AcquireStmt(raw, sql));
    if (stmt == nullptr) {
        return core::Result<MySqlResultSet>::Fail(
            core::Error(MapMySqlError(err_.code), err_.message, core::domain::kData));
    }
    // 执行失败必须丢弃该语句的缓存项（坏状态不可复用）；连接类错误还要清空整表。
    auto stmt_fail = [&](unsigned int code, std::string_view msg) {
        const core::Error e = RecordError(code, msg);
        DropStmt(sql);
        if (broken_) ClearStmtCache();
        return core::Result<MySqlResultSet>::Fail(e);
    };

    if (mysql_stmt_param_count(stmt) != params.size()) {
        DropStmt(sql);
        return core::Result<MySqlResultSet>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "bind count mismatch with placeholders"));
    }

    // ---- 绑定入参 ----
    std::vector<MYSQL_BIND> binds(params.size());
    std::vector<std::int64_t> ints(params.size());
    std::vector<double> reals(params.size());
    std::vector<unsigned long> lens(params.size());
    // is_null / error 必须是可取地址的逐元素数组；my_bool 即 char（mysql.h 定义），
    // 故 vector<my_bool> 就是普通 vector<char>，不会退化成 vector<bool> 的位代理。
    std::vector<my_bool> nulls(params.size(), 0);
    std::memset(binds.data(), 0, binds.size() * sizeof(MYSQL_BIND));

    for (std::size_t i = 0; i < params.size(); ++i) {
        const MySqlValue& v = params[i];
        nulls[i] = v.is_null() ? 1 : 0;
        binds[i].is_null = &nulls[i];
        binds[i].length = &lens[i];
        switch (v.type) {
            case MySqlValueType::Null:
                binds[i].buffer_type = MYSQL_TYPE_NULL;
                break;
            case MySqlValueType::Int64:
                ints[i] = v.i64;
                binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                binds[i].buffer = &ints[i];
                binds[i].is_unsigned = 0;
                break;
            case MySqlValueType::Uint64:
                ints[i] = v.i64;
                binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                binds[i].buffer = &ints[i];
                binds[i].is_unsigned = 1;
                break;
            case MySqlValueType::Double:
                reals[i] = v.dbl;
                binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                binds[i].buffer = &reals[i];
                break;
            case MySqlValueType::Text:
            case MySqlValueType::Binary:
                lens[i] = static_cast<unsigned long>(v.str.size());
                // Text 走连接字符集（utf8mb4）；Binary 走 BLOB，不做字符集转换（二进制安全）
                binds[i].buffer_type =
                    (v.type == MySqlValueType::Binary) ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
                binds[i].buffer = const_cast<char*>(v.str.data());
                binds[i].buffer_length = lens[i];
                break;
        }
    }
    if (!params.empty() && mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        return stmt_fail(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
    }
    if (mysql_stmt_execute(stmt) != 0) {
        return stmt_fail(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
    }

    MySqlResultSet out;
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (meta == nullptr) {
        // DML / DDL：无结果集（句柄保留在缓存中供下次复用）
        out.affected_rows = static_cast<std::uint64_t>(mysql_stmt_affected_rows(stmt));
        out.insert_id = static_cast<std::uint64_t>(mysql_stmt_insert_id(stmt));
        return core::Result<MySqlResultSet>::Ok(std::move(out));
    }
    ResultGuard meta_guard;
    meta_guard.res = meta;

    out.has_result_set = true;
    const unsigned int ncols = mysql_num_fields(meta);
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);
    out.columns.resize(ncols);
    for (unsigned int i = 0; i < ncols; ++i) {
        out.columns[i].name = (fields[i].name != nullptr) ? fields[i].name : "";
    }

    const my_bool one = 1;
    mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &one);
    if (mysql_stmt_store_result(stmt) != 0) {
        return stmt_fail(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
    }

    // 逐列按 max_length 分配缓冲（store_result 之后 max_length 已是真实最大长度）
    std::vector<std::vector<char>> buffers(ncols);
    std::vector<unsigned long> out_lens(ncols);
    std::vector<my_bool> out_nulls(ncols, 0);
    std::vector<my_bool> out_errs(ncols, 0);
    std::vector<MYSQL_BIND> rbind(ncols);
    std::memset(rbind.data(), 0, rbind.size() * sizeof(MYSQL_BIND));
    for (unsigned int i = 0; i < ncols; ++i) {
        const unsigned long cap = (fields[i].max_length > 0) ? fields[i].max_length + 1 : 64;
        buffers[i].assign(cap, '\0');
        rbind[i].buffer_type = MYSQL_TYPE_STRING;
        rbind[i].buffer = buffers[i].data();
        rbind[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
        rbind[i].length = &out_lens[i];
        rbind[i].is_null = &out_nulls[i];
        rbind[i].error = &out_errs[i];
    }
    if (mysql_stmt_bind_result(stmt, rbind.data()) != 0) {
        return stmt_fail(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
    }

    for (;;) {
        const int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) break;
        if (rc == 1) {
            return stmt_fail(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
        }
        MySqlRow r;
        r.values.reserve(ncols);
        for (unsigned int i = 0; i < ncols; ++i) {
            if (out_nulls[i] != 0) {
                r.values.push_back(MySqlValue::Null());
                continue;
            }
            std::string text;
            if (rc == MYSQL_DATA_TRUNCATED && out_lens[i] + 1 > buffers[i].size()) {
                // 超出预估长度：按真实长度重取该列（长 payload / 长文本场景）
                std::vector<char> big(out_lens[i] + 1, '\0');
                MYSQL_BIND one_bind;
                std::memset(&one_bind, 0, sizeof(one_bind));
                unsigned long real_len = 0;
                my_bool nb = 0;
                one_bind.buffer_type = MYSQL_TYPE_STRING;
                one_bind.buffer = big.data();
                one_bind.buffer_length = static_cast<unsigned long>(big.size());
                one_bind.length = &real_len;
                one_bind.is_null = &nb;
                if (mysql_stmt_fetch_column(stmt, &one_bind, i, 0) != 0) {
                    return stmt_fail(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
                }
                text.assign(big.data(), real_len);
            } else {
                text.assign(buffers[i].data(), out_lens[i]);
            }
            r.values.push_back(MySqlValue::Text(std::move(text)));
        }
        out.rows.push_back(std::move(r));
    }
    out.affected_rows = static_cast<std::uint64_t>(mysql_stmt_affected_rows(stmt));
    // 结果已取完 -> 释放服务端结果集，句柄可安全复用（缓存命中路径的前提）
    (void)mysql_stmt_free_result(stmt);
    return core::Result<MySqlResultSet>::Ok(std::move(out));
}

core::Result<void> MySqlConnection::Begin() { return ToVoid(Execute("START TRANSACTION")); }
core::Result<void> MySqlConnection::Commit() { return ToVoid(Execute("COMMIT")); }
core::Result<void> MySqlConnection::Rollback() { return ToVoid(Execute("ROLLBACK")); }

core::Result<void> EnsureDatabaseExists(const ShardEndpoint& ep, const MySqlConfig& cfg,
                                        std::string_view password) {
    if (!IsSafeIdentifier(ep.database)) {
        return core::Result<void>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "unsafe database name"));
    }
    // 不指定库名连接（引导连接），建库后再由业务连接选库。
    ShardEndpoint boot = ep;
    boot.database.clear();
    auto conn = MySqlConnection::Open(boot, cfg, password);
    if (!conn.HasValue()) return core::Result<void>::Fail(conn.Err());
    auto rs = std::move(conn).Value().Execute("CREATE DATABASE IF NOT EXISTS `" + ep.database +
                                   "` DEFAULT CHARSET=utf8mb4");
    if (!rs.HasValue()) return core::Result<void>::Fail(rs.Err());
    return core::Result<void>::Ok();
}

}  // namespace mmo::data::mysql
