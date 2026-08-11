// data/mysql_proxy/mysql_backing_store.cpp — [PRODUCTION] MySQL 落库后端实现
// 仅在 CAMI_BUILD_MODULES=ON 编译 (vcpkg: libmysql)。OFF 构建本文件为空 TU。
//
// 通过 ShardingSphere 代理 (MySQL 协议) 访问分片库; 代理按 player_id%8 路由。
// 使用 libmysql C API + 预处理语句 (防注入)。
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

MySQLBackingStore::~MySQLBackingStore() {
    std::lock_guard<std::mutex> lk(mu_);
    if (conn_) { mysql_close(conn_); conn_ = nullptr; }
}

std::optional<std::string> MySQLBackingStore::Load(std::string_view key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_conn()) return std::nullopt;

    // key 形式为 "player:<id>" (与缓存 key 一致); 剥前缀取数值 id (schema 中 player_id 为分片键)。
    std::string pid(key);
    constexpr char kPrefix[] = "player:";
    if (pid.size() > sizeof(kPrefix) - 1 && pid.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0)
        pid = pid.substr(sizeof(kPrefix) - 1);
    // strtoull 归一为纯数值白名单再拼 SQL: 与 Store 的预处理绑定同级别防注入
    // (杜绝任意 key 内容进入 SQL 文本)。
    unsigned long long pid_val = std::strtoull(pid.c_str(), nullptr, 10);
    std::string sql = "SELECT payload FROM player_base WHERE player_id = " +
                      std::to_string(pid_val) + " LIMIT 1";
    if (mysql_real_query(conn_, sql.c_str(), sql.size()) != 0) {
        // 查询失败: 返回空 (回源失败, CacheProxy 视为未命中)
        return std::nullopt;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    std::optional<std::string> out;
    if (row && row[0]) out = std::string(row[0]);
    mysql_free_result(res);
    return out;
}

void MySQLBackingStore::Store(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_conn()) return;

    // key 形式为 "player:<id>"; 剥前缀取数值 id (schema 中 player_id 为 BIGINT UNSIGNED 分片键)。
    std::string pid(key);
    constexpr char kPrefix[] = "player:";
    if (pid.size() > sizeof(kPrefix) - 1 && pid.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0)
        pid = pid.substr(sizeof(kPrefix) - 1);
    // player_id 为 BIGINT UNSIGNED (64 位), 统一 strtoull 归一为纯数值 (防注入 + 防 32 位截断)。
    unsigned long long pid_val = std::strtoull(pid.c_str(), nullptr, 10);

    if (value.empty()) {
        // 空 payload = 删除语义 (CacheProxy::Delete 双删调用): DELETE 行, 而非写入空串。
        MYSQL_STMT* stmt = mysql_stmt_init(conn_);
        if (!stmt) return;
        const char* q = "DELETE FROM player_base WHERE player_id = ?";
        if (mysql_stmt_prepare(stmt, q, strlen(q)) != 0) { mysql_stmt_close(stmt); return; }
        MYSQL_BIND bind[1];
        memset(bind, 0, sizeof(bind));
        bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[0].buffer = &pid_val;
        bind[0].is_unsigned = 1;
        if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
            mysql_stmt_close(stmt);
            return;
        }
        mysql_stmt_close(stmt);
        return;
    }

    // 用预处理语句防注入 (payload 为二进制 blob, 用 ? 绑定)
    MYSQL_STMT* stmt = mysql_stmt_init(conn_);
    if (!stmt) return;
    const char* q = "REPLACE INTO player_base (player_id, payload) VALUES (?, ?)";
    if (mysql_stmt_prepare(stmt, q, strlen(q)) != 0) { mysql_stmt_close(stmt); return; }

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

    if (mysql_stmt_bind_param(stmt, bind) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return;
    }
    mysql_stmt_close(stmt);
}

}  // namespace mysql_proxy
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
