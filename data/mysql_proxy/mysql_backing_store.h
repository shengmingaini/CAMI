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

struct st_mysql;   // 前置声明, 避免暴露 <mysql.h> 给包含方

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
};

}  // namespace mysql_proxy
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
