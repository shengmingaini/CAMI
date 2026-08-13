// data/mysql_proxy/mysql_backing_store.cpp — [PRODUCTION] MySQL 落库后端实现
// 仅在 CAMI_BUILD_MODULES=ON 编译 (vcpkg: libmariadb)。OFF 构建本文件为空 TU。
//
// 通过 ShardingSphere 代理 (MySQL 协议) 访问分片库; 代理按 player_id%8 路由。
// 使用 libmariadb C API + 预处理语句 (防注入)。
#include "data/mysql_proxy/mysql_backing_store.h"

#ifdef CAMI_BUILD_MODULES

#include <mysql.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace cami {
namespace data {
namespace mysql_proxy {

MySQLBackingStore::MySQLBackingStore(const std::string& proxy_host, uint16_t proxy_port,
                                     const std::string& user, const std::string& pass,
                                     const std::string& db)
    : host_(proxy_host), port_(proxy_port), user_(user), pass_(pass), db_(db) {
    conn_ = mysql_init(nullptr);
    if (!conn_) throw std::runtime_error("[MySQLBackingStore] mysql_init failed");
    configure_opts();
    // 连接 ShardingSphere 代理
    if (!mysql_real_connect(conn_, host_.c_str(), user_.c_str(), pass_.c_str(),
                            db_.c_str(), port_, nullptr, 0)) {
        std::string err = mysql_error(conn_);
        mysql_close(conn_);
        conn_ = nullptr;
        throw std::runtime_error("[MySQLBackingStore] connect failed: " + err);
    }
}

void MySQLBackingStore::configure_opts() {
    // 连接/读写超时, 防 ShardingSphere 代理无响应时阻塞调用线程 (gRPC 有 deadline, 这里更早兜底)
    unsigned int connect_timeout = 3;
    mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
    unsigned int read_timeout = 5;
    mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
    unsigned int write_timeout = 5;
    mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);
}

bool MySQLBackingStore::ensure_conn() {
    if (!conn_) return false;
    if (mysql_ping(conn_) == 0) return true;
    // 断线: 显式重连 (Data Service 常驻进程, 代理重启/网络抖动后必须能恢复)
    mysql_close(conn_);
    CloseStmts();  // 预编译语句与旧连接绑定, 重连后全部失效, 必须重建
    conn_ = mysql_init(nullptr);
    if (!conn_) return false;
    configure_opts();
    if (!mysql_real_connect(conn_, host_.c_str(), user_.c_str(), pass_.c_str(),
                            db_.c_str(), port_, nullptr, 0)) {
        std::string err = mysql_error(conn_);
        mysql_close(conn_);
        conn_ = nullptr;
        std::cerr << "[MySQLBackingStore] reconnect failed: " << err << std::endl;
        return false;
    }
    return true;
}

// [2026-08-12 优化] statement 预编译缓存: 同构 SQL 只 prepare 一次, 后续 execute 复用。
// 调用方持锁 (mu_) 调用; 返回 nullptr 表示 prepare 失败 (调用方按失败处理)。
MYSQL_STMT* MySQLBackingStore::GetStmt(MYSQL_STMT*& cache, const char* sql) {
    if (cache) return cache;  // 已预编译, 直接复用 (5 万次落库只需 1 次 prepare)
    cache = mysql_stmt_init(conn_);
    if (!cache) return nullptr;
    if (mysql_stmt_prepare(cache, sql, strlen(sql)) != 0) {
        std::cerr << "[MySQLBackingStore] stmt prepare failed: " << mysql_error(conn_)
                  << " sql=" << sql << std::endl;
        mysql_stmt_close(cache);
        cache = nullptr;
        return nullptr;
    }
    return cache;
}

void MySQLBackingStore::CloseStmts() {
    if (upsert_stmt_) { mysql_stmt_close(upsert_stmt_); upsert_stmt_ = nullptr; }
    if (delete_stmt_) { mysql_stmt_close(delete_stmt_); delete_stmt_ = nullptr; }
    if (cas_upd_stmt_) { mysql_stmt_close(cas_upd_stmt_); cas_upd_stmt_ = nullptr; }
    if (cas_sel_stmt_) { mysql_stmt_close(cas_sel_stmt_); cas_sel_stmt_ = nullptr; }
    if (cas_ins_stmt_) { mysql_stmt_close(cas_ins_stmt_); cas_ins_stmt_ = nullptr; }
}

MySQLBackingStore::~MySQLBackingStore() {
    std::lock_guard<std::mutex> lk(mu_);
    CloseStmts();
    if (conn_) { mysql_close(conn_); conn_ = nullptr; }
}

namespace {
// 剥 "player:" 前缀, 返回数值 id 字符串 (非数值则原样返回, 由 strtoull 归一)
std::string StripKeyPrefix(std::string_view key) {
    std::string pid(key);
    constexpr char kPrefix[] = "player:";
    if (pid.size() > sizeof(kPrefix) - 1 && pid.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0)
        pid = pid.substr(sizeof(kPrefix) - 1);
    return pid;
}
}  // namespace

std::optional<std::string> MySQLBackingStore::Load(std::string_view key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_conn()) return std::nullopt;

    // key 形式为 "player:<id>" (与缓存 key 一致); 剥前缀取数值 id。
    // strtoull 归一为纯数值白名单再拼 SQL: 与写路径预处理同级别防注入。
    unsigned long long pid_val =
        std::strtoull(StripKeyPrefix(key).c_str(), nullptr, 10);
    std::string sql = "SELECT payload FROM player_state WHERE player_id = " +
                      std::to_string(pid_val) + " LIMIT 1";
    if (mysql_real_query(conn_, sql.c_str(), sql.size()) != 0) {
        // 查询失败: 返回空 (回源失败, CacheProxy 视为未命中)
        return std::nullopt;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    std::optional<std::string> out;
    if (row && row[0]) {
        // payload 为二进制 blob 可能含 \0: 必须用 mysql_fetch_lengths 的实际长度, 防 strlen 截断。
        unsigned long* lens = mysql_fetch_lengths(res);
        out = std::string(row[0], lens ? lens[0] : strlen(row[0]));
    }
    mysql_free_result(res);
    return out;
}

void MySQLBackingStore::Store(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_conn()) return;

    // key 形式为 "player:<id>"; 剥前缀取数值 id (schema 中 player_id 为 BIGINT UNSIGNED 分片键)。
    unsigned long long pid_val =
        std::strtoull(StripKeyPrefix(key).c_str(), nullptr, 10);

    if (value.empty()) {
        // 空 payload = 删除语义 (防御; 常规双删走 Delete 方法)。
        // 注意: 此处已持有 mu_, 不能调 Delete() (会重入加锁死锁), 内联 DELETE (复用 delete_stmt_)。
        MYSQL_STMT* stmt = GetStmt(delete_stmt_,
                                   "DELETE FROM player_state WHERE player_id = ?");
        if (!stmt) return;
        MYSQL_BIND bind[1];
        memset(bind, 0, sizeof(bind));
        bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[0].buffer = &pid_val;
        bind[0].is_unsigned = 1;
        if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
            return;  // stmt 缓存保留, 下次复用
        }
        return;
    }

    // 无条件 UPSERT (Put 语义 = last-write-wins): 新行 version=1, 已存在 version+1。
    // 目标表 player_state (方案 B: Data Service 序列化行专用, 与 player_base 结构化列分离)。
    MYSQL_STMT* stmt = GetStmt(upsert_stmt_,
        "INSERT INTO player_state (player_id, payload, version) VALUES (?, ?, 1) "
        "ON DUPLICATE KEY UPDATE payload = VALUES(payload), version = version + 1");
    if (!stmt) return;

    MYSQL_BIND bind[2];
    memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &pid_val;
    bind[0].is_unsigned = 1;

    const char* pdata = value.data();
    unsigned long plen = static_cast<unsigned long>(value.size());
    bind[1].buffer_type = MYSQL_TYPE_MEDIUM_BLOB;
    bind[1].buffer = const_cast<char*>(pdata);
    bind[1].buffer_length = plen;
    bind[1].length = &plen;

    (void)mysql_stmt_bind_param(stmt, bind);
    (void)mysql_stmt_execute(stmt);
}

std::optional<redis_proxy::StoreRow> MySQLBackingStore::LoadWithVersion(std::string_view key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_conn()) return std::nullopt;

    unsigned long long pid_val =
        std::strtoull(StripKeyPrefix(key).c_str(), nullptr, 10);
    std::string sql = "SELECT payload, version FROM player_state WHERE player_id = " +
                      std::to_string(pid_val) + " LIMIT 1";
    if (mysql_real_query(conn_, sql.c_str(), sql.size()) != 0) return std::nullopt;
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    std::optional<redis_proxy::StoreRow> out;
    if (row && row[0]) {
        unsigned long* lens = mysql_fetch_lengths(res);  // 二进制 blob 用实际长度
        redis_proxy::StoreRow r;
        r.payload = std::string(row[0], lens ? lens[0] : strlen(row[0]));
        r.version = row[1] ? std::strtoull(row[1], nullptr, 10) : 0;
        out = std::move(r);
    }
    mysql_free_result(res);
    return out;
}

bool MySQLBackingStore::CasStore(std::string_view key, std::string_view value,
                                 uint64_t expected_version) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_conn()) return false;

    unsigned long long pid_val =
        std::strtoull(StripKeyPrefix(key).c_str(), nullptr, 10);
    const char* pdata = value.data();
    unsigned long plen = static_cast<unsigned long>(value.size());
    unsigned long long exp = expected_version;

    // 1) 版本条件更新: 仅当 DB version == expected 才写并 +1 (affected_rows==1 成功)
    MYSQL_STMT* stmt = GetStmt(cas_upd_stmt_,
        "UPDATE player_state SET payload = ?, version = version + 1 "
        "WHERE player_id = ? AND version = ?");
    if (!stmt) return false;
    MYSQL_BIND bind[3];
    memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_MEDIUM_BLOB;
    bind[0].buffer = const_cast<char*>(pdata);
    bind[0].buffer_length = plen;
    bind[0].length = &plen;
    bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[1].buffer = &pid_val;
    bind[1].is_unsigned = 1;
    bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[2].buffer = &exp;
    bind[2].is_unsigned = 1;
    bool upd_ok = mysql_stmt_bind_param(stmt, bind) == 0 && mysql_stmt_execute(stmt) == 0;
    my_ulonglong affected = upd_ok ? mysql_stmt_affected_rows(stmt) : 0;
    if (affected == 1) return true;

    // 2) affected==0: 行不存在 或 版本冲突。查存在性区分。
    MYSQL_STMT* chk = GetStmt(cas_sel_stmt_,
                              "SELECT 1 FROM player_state WHERE player_id = ? LIMIT 1");
    if (!chk) return false;
    MYSQL_BIND cb[1];
    memset(cb, 0, sizeof(cb));
    cb[0].buffer_type = MYSQL_TYPE_LONGLONG;
    cb[0].buffer = &pid_val;
    cb[0].is_unsigned = 1;
    bool chk_ok = mysql_stmt_bind_param(chk, cb) == 0 && mysql_stmt_execute(chk) == 0;
    my_ulonglong exists = 0;
    if (chk_ok) {
        mysql_stmt_store_result(chk);
        exists = mysql_stmt_num_rows(chk);
    }
    if (exists == 0) {
        // 行不存在: 仅允许 expected=0 创建 (version 0 -> 1)
        if (expected_version != 0) return false;
        MYSQL_STMT* ins = GetStmt(cas_ins_stmt_,
            "INSERT INTO player_state (player_id, payload, version) VALUES (?, ?, 1)");
        if (!ins) return false;
        MYSQL_BIND ib[2];
        memset(ib, 0, sizeof(ib));
        ib[0].buffer_type = MYSQL_TYPE_LONGLONG;
        ib[0].buffer = &pid_val;
        ib[0].is_unsigned = 1;
        ib[1].buffer_type = MYSQL_TYPE_MEDIUM_BLOB;
        ib[1].buffer = const_cast<char*>(pdata);
        ib[1].buffer_length = plen;
        ib[1].length = &plen;
        bool ins_ok = mysql_stmt_bind_param(ins, ib) == 0 && mysql_stmt_execute(ins) == 0;
        my_ulonglong ia = ins_ok ? mysql_stmt_affected_rows(ins) : 0;
        return ins_ok && ia == 1;  // 并发创建 PK 冲突 -> affected=0 -> false
    }
    return false;  // 行存在但版本不匹配 = 冲突
}

void MySQLBackingStore::Delete(std::string_view key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_conn()) return;
    unsigned long long pid_val =
        std::strtoull(StripKeyPrefix(key).c_str(), nullptr, 10);
    MYSQL_STMT* stmt = GetStmt(delete_stmt_,
                               "DELETE FROM player_state WHERE player_id = ?");
    if (!stmt) return;
    MYSQL_BIND bind[1];
    memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &pid_val;
    bind[0].is_unsigned = 1;
    (void)mysql_stmt_bind_param(stmt, bind);
    (void)mysql_stmt_execute(stmt);
}

}  // namespace mysql_proxy
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
