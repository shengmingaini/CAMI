#pragma once
// ============================================================================
// data/rocksdb_proxy/rocksdb_backing_store.h — [PRODUCTION] RocksDB 嵌入式 KV 落库后端 (阶段 A W4)
// ----------------------------------------------------------------------------
// 设计目标 (radical-optimization-blueprint ADR-014 「数据层」):
//   以单进程内嵌 RocksDB 取代 MySQL + ShardingSphere + Redis 外部栈, 把 D3/D6 的
//   "缓存 + 回源分片库" 收敛为单一嵌入式 KV, 直接消灭 Dev-Std 的 30 容器 / ~12GB
//   外部依赖, 让最小起步(Dev-Mini)只需 1 个进程 ≈ 数百 MB。
//
//   三列族 (Column Family) 职责分离:
//     kCfState : key -> 当前物化状态 (Cap'n Proto blob) + 版本前缀
//     kCfMeta  : player_id -> 64 位版本号 (CAS 裁决源, 避免解析 state 取版本)
//     kCfEvents: 追加不可变事件 (事件溯源): key = player_id|seq, value = 事件 blob
//                WAL 保证每条事件落盘即持久, 可从头重放重建任意时刻状态 (自修复/迁移)。
//
//   红线延续: GameNode 不直连; 仅 Data Service 经此落库 (缓存未命中回源 + 异步落库)。
//   版本条件写走 kCfMeta 的 read-modify-write, 用分桶互斥锁 (非全局锁) 保护, 满足
//   架构「禁止全局锁」硬约束。
//
//   仅在 CAMI_BUILD_MODULES=ON (vcpkg: rocksdb) 下编译; OFF 构建看不到本文件。
//   实现见 rocksdb_backing_store.cpp (同样 #ifdef 守护, MODULES=ON 真编译交 CI)。
// ============================================================================
#ifdef CAMI_BUILD_MODULES

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <rocksdb/db.h>

#include "data/redis_proxy/cache_proxy.h"   // BackingStore, StoreRow

namespace cami {
namespace data {
namespace rocksdb_proxy {

class RocksDBBackingStore : public redis_proxy::BackingStore {
public:
    // path: 本地 sst + WAL 目录 (单进程独占, 嵌在 Data Service 内)
    // archive_uri: 可选 MySQL 只读归档 URI (降级/冷数据回源, 见 blueprint "MySQL 降级只读归档")
    explicit RocksDBBackingStore(const std::string& path,
                                 const std::string& archive_uri = "");
    ~RocksDBBackingStore() override;

    // --- BackingStore 接口 (对齐 MySQLBackingStore, CacheProxy 可直接驱动) ---
    std::optional<std::string> Load(std::string_view key) override;
    void Store(std::string_view key, std::string_view value) override;
    std::optional<redis_proxy::StoreRow> LoadWithVersion(std::string_view key) override;
    bool CasStore(std::string_view key, std::string_view value,
                  uint64_t expected_version) override;
    void Delete(std::string_view key) override;

    // --- 事件溯源 (W4 新增能力, 基类接口未含, 由 Data Service 直接调用) ---
    // 追加一条不可变事件; 返回 false = 写入失败 (熔断应触发回退)。
    bool AppendEvent(std::string_view player_id, std::string_view event_blob);
    // 从 kCfEvents 重放某 player 的事件流, 重建物化状态到 kCfState。
    // [PROTOTYPE] 重放语义由玩法层提供 apply 回调; 此处仅做列族遍历骨架。
    bool RebuildFromEvents(std::string_view player_id);

    bool healthy() const { return db_ != nullptr; }

private:
    // 分桶互斥锁 (64 桶, 按 key 哈希): 仅保护 kCfMeta 的 read-modify-write,
    // 避免单全局锁 (架构红线「禁止全局锁」)。state/events 的原子 Put 无需锁。
    static constexpr std::size_t kMetaShards = 64;
    struct MetaShard { std::mutex mu; };
    std::array<MetaShard, kMetaShards> meta_shards_;

    std::unique_ptr<rocksdb::DB> db_;  // 拥有 DB (RocksDB 11.x 的 Open 返回 unique_ptr)
    rocksdb::ColumnFamilyHandle* cf_default_ = nullptr;  // 默认 CF (必须保存并析构, 否则 handle 泄漏)
    rocksdb::ColumnFamilyHandle* cf_state_ = nullptr;
    rocksdb::ColumnFamilyHandle* cf_meta_ = nullptr;
    rocksdb::ColumnFamilyHandle* cf_events_ = nullptr;
    std::string archive_uri_;
};

}  // namespace rocksdb_proxy
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
