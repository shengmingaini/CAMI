#pragma once
// ============================================================================
// data/mysql_proxy/mysql_backing_store.h — [PRODUCTION] MySQL 落库后端
// ----------------------------------------------------------------------------
// 实现 BackingStore 抽象: Load/Store 玩家行到分片 MySQL。
// 通过 ShardingSphere 代理 (MySQL 协议) 访问, 由代理做 player_id%8 分片路由,
// 因此本类只连代理地址, 不感知 8 个物理分库。
//
// 仅在 CAMI_BUILD_MODULES=ON (vcpkg: libmysql C 客户端) 下编译。
// 红线: GameNode 不直连; 仅 Data Service 经此落库 (缓存未命中回源 + 异步落库)。
// ============================================================================
#ifdef CAMI_BUILD_MODULES

#include <mutex>
#include <optional>
#include <string>

#include "data/redis_proxy/cache_proxy.h"   // BackingStore

struct st_mysql;          // 前置声明, 避免暴露 <mysql.h> 给包含方
struct st_mysql_stmt;     // MYSQL_STMT 的底层结构 (同上, 预编译语句缓存用)

namespace cami {
namespace data {
namespace mysql_proxy {

class MySQLBackingStore : public redis_proxy::BackingStore {
public:
    // proxy_uri 例: "tcp://127.0.0.1:3309" (ShardingSphere 代理)
    // db: 逻辑库名 (cami_db)
    MySQLBackingStore(const std::string& proxy_host, uint16_t proxy_port,
                      const std::string& user, const std::string& pass,
                      const std::string& db);
    ~MySQLBackingStore() override;

    std::optional<std::string> Load(std::string_view key) override;
    void Store(std::string_view key, std::string_view value) override;
    // D6 方案 B: 回源带 DB 权威版本 / 版本条件写 / 删行
    std::optional<redis_proxy::StoreRow> LoadWithVersion(std::string_view key) override;
    bool CasStore(std::string_view key, std::string_view value,
                  uint64_t expected_version) override;
    void Delete(std::string_view key) override;

private:
    // 连接参数缓存, 供断线后重连 (ShardingSphere 代理重启场景)
    std::string host_;
    uint16_t port_ = 0;
    std::string user_;
    std::string pass_;
    std::string db_;
    st_mysql* conn_ = nullptr;
    mutable std::mutex mu_;

    // 配置连接/读写超时 (防 DB 无响应时阻塞调用线程)
    void configure_opts();
    // 探活; 断线则重连一次; 返回 false = 连接不可用
    bool ensure_conn();
    // [2026-08-12 优化] statement 预编译缓存: prepare 一次, execute 复用
    // (批量落库 5 万 key 从 5 万次 prepare+网络往返降到 1 次 prepare)。
    // 缓存与连接绑定: 重连后旧 stmt 失效, ensure_conn 会先 CloseStmts。
    struct st_mysql_stmt* GetStmt(struct st_mysql_stmt*& cache, const char* sql);
    void CloseStmts();

    struct st_mysql_stmt* upsert_stmt_ = nullptr;  // Store: INSERT ... ON DUPLICATE KEY UPDATE
    struct st_mysql_stmt* delete_stmt_ = nullptr;  // Delete / Store 空值路径: DELETE WHERE player_id=?
    struct st_mysql_stmt* cas_upd_stmt_ = nullptr; // CasStore: UPDATE ... WHERE player_id=? AND version=?
    struct st_mysql_stmt* cas_sel_stmt_ = nullptr; // CasStore 存在性: SELECT 1 WHERE player_id=?
    struct st_mysql_stmt* cas_ins_stmt_ = nullptr; // CasStore 新行: INSERT (player_id, payload, version)
};

}  // namespace mysql_proxy
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
