// ============================================================================
// data/rocksdb_proxy/rocksdb_backing_store.cpp — [PRODUCTION] RocksDB 落库后端实现 (阶段 A W4)
// ----------------------------------------------------------------------------
// 仅 CAMI_BUILD_MODULES=ON 编译 (vcpkg: rocksdb)。OFF 构建此 TU 为空。
// 实现 BackingStore 五接口 + 事件溯源 (AppendEvent / RebuildFromEvents)。
// 版本裁决源 = kCfMeta (64 位), CAS 走分桶互斥锁 read-modify-write (非全局锁)。
// ============================================================================
#ifdef CAMI_BUILD_MODULES

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "data/rocksdb_proxy/rocksdb_backing_store.h"

namespace cami {
namespace data {
namespace rocksdb_proxy {

// --- 版本编解码 (8 字节小端, 存于 kCfMeta) ---
static std::string EncodeVersion(uint64_t v) {
    std::string b(8, '\0');
    std::memcpy(b.data(), &v, sizeof(v));
    return b;
}
static uint64_t DecodeVersion(const std::string& b) {
    uint64_t v = 0;
    if (b.size() == 8) std::memcpy(&v, b.data(), 8);
    return v;
}

// --- 状态 value 布局: [8B version][payload] (state 与 meta 同源, 读路径可一次取齐) ---
static std::string PackState(uint64_t version, std::string_view payload) {
    std::string out(8, '\0');
    std::memcpy(out.data(), &version, 8);
    out.append(payload.data(), payload.size());
    return out;
}
static void UnpackState(const std::string& raw, uint64_t& version, std::string& payload) {
    if (raw.size() >= 8) {
        std::memcpy(&version, raw.data(), 8);
        payload.assign(raw.data() + 8, raw.size() - 8);
    } else {
        version = 0; payload.clear();
    }
}

static std::size_t KeyShard(std::string_view key) {
    // FNV-1a 32-bit -> 取模 64 桶
    uint32_t h = 2166136261u;
    for (char c : key) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
    return static_cast<std::size_t>(h & 63u);
}

RocksDBBackingStore::RocksDBBackingStore(const std::string& path,
                                         const std::string& archive_uri)
    : archive_uri_(archive_uri) {
    rocksdb::DBOptions db_opts;
    db_opts.create_if_missing = true;
    db_opts.create_missing_column_families = true;
    db_opts.error_if_exists = false;

    // 默认 CF + 三个业务列族
    std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs;
    cf_descs.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions{});
    cf_descs.emplace_back("state", rocksdb::ColumnFamilyOptions{});
    cf_descs.emplace_back("meta",  rocksdb::ColumnFamilyOptions{});
    cf_descs.emplace_back("events", rocksdb::ColumnFamilyOptions{});

    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status s = rocksdb::DB::Open(db_opts, path, cf_descs, &handles, &db);
    if (!s.ok() || !db) {
        // 打开失败: 不抛构造期异常 (避免进程启动崩溃), 标记 unhealthy 由 Health 探针暴露。
        db_ = nullptr;
        return;
    }
    db_ = std::move(db);
    // handles 顺序与 cf_descs 一致: 0=default, 1=state, 2=meta, 3=events
    cf_default_ = handles.at(0);
    cf_state_  = handles.at(1);
    cf_meta_   = handles.at(2);
    cf_events_ = handles.at(3);
}

RocksDBBackingStore::~RocksDBBackingStore() {
    if (db_) {
        // 先删全部 CF handle (含 default), 再关 DB。RocksDB 要求调用方显式释放
        // 所有 ColumnFamilyHandle, 否则 default CF handle 泄漏 / 析构期断言失败。
        delete cf_default_;
        delete cf_state_;
        delete cf_meta_;
        delete cf_events_;
        // db_ 为 unique_ptr, 在作用域末尾自动析构 (须在所有 CF handle 释放之后,
        // 满足 RocksDB 的释放顺序要求: CF handle -> DB)。
    }
}

std::optional<std::string> RocksDBBackingStore::Load(std::string_view key) {
    if (!db_) return std::nullopt;
    std::string raw;
    rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), cf_state_,
                                  rocksdb::Slice(key.data(), key.size()), &raw);
    if (s.IsNotFound()) return std::nullopt;
    if (!s.ok()) throw std::runtime_error("RocksDB Load failed: " + s.ToString());
    uint64_t ver = 0; std::string payload;
    UnpackState(raw, ver, payload);
    return payload;
}

void RocksDBBackingStore::Store(std::string_view key, std::string_view value) {
    if (!db_) return;
    // 无条件 UPSERT: 读旧版本 +1, 无则置 1
    std::size_t shard = KeyShard(key);
    std::lock_guard<std::mutex> lk(meta_shards_[shard].mu);
    std::string mraw;
    uint64_t ver = 0;
    (void)db_->Get(rocksdb::ReadOptions(), cf_meta_,
                    rocksdb::Slice(key.data(), key.size()), &mraw);
    ver = DecodeVersion(mraw) + 1;
    rocksdb::WriteBatch batch;
    batch.Put(cf_state_, rocksdb::Slice(key.data(), key.size()), PackState(ver, value));
    batch.Put(cf_meta_,  rocksdb::Slice(key.data(), key.size()), EncodeVersion(ver));
    rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
    if (!s.ok()) throw std::runtime_error("RocksDB Store failed: " + s.ToString());
}

std::optional<redis_proxy::StoreRow> RocksDBBackingStore::LoadWithVersion(std::string_view key) {
    if (!db_) return std::nullopt;
    std::string raw;
    rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), cf_state_,
                                 rocksdb::Slice(key.data(), key.size()), &raw);
    if (s.IsNotFound()) return std::nullopt;
    if (!s.ok()) throw std::runtime_error("RocksDB LoadWithVersion failed: " + s.ToString());
    uint64_t ver = 0; std::string payload;
    UnpackState(raw, ver, payload);
    return redis_proxy::StoreRow{std::move(payload), ver};
}

bool RocksDBBackingStore::CasStore(std::string_view key, std::string_view value,
                                    uint64_t expected_version) {
    if (!db_) return false;
    std::size_t shard = KeyShard(key);
    std::lock_guard<std::mutex> lk(meta_shards_[shard].mu);  // 分桶锁 (非全局锁)
    std::string mraw;
    rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), cf_meta_,
                                 rocksdb::Slice(key.data(), key.size()), &mraw);
    if (s.IsNotFound()) {
        if (expected_version != 0) return false;  // 新行只接受 expected=0
        uint64_t ver = 1;
        rocksdb::WriteBatch batch;
        batch.Put(cf_state_, rocksdb::Slice(key.data(), key.size()), PackState(ver, value));
        batch.Put(cf_meta_,  rocksdb::Slice(key.data(), key.size()), EncodeVersion(ver));
        return db_->Write(rocksdb::WriteOptions(), &batch).ok();
    }
    if (!s.ok()) return false;
    uint64_t cur = DecodeVersion(mraw);
    if (cur != expected_version) return false;  // 版本冲突 -> 不覆盖 (affected_rows==0 语义)
    uint64_t ver = cur + 1;
    rocksdb::WriteBatch batch;
    batch.Put(cf_state_, rocksdb::Slice(key.data(), key.size()), PackState(ver, value));
    batch.Put(cf_meta_,  rocksdb::Slice(key.data(), key.size()), EncodeVersion(ver));
    return db_->Write(rocksdb::WriteOptions(), &batch).ok();
}

void RocksDBBackingStore::Delete(std::string_view key) {
    if (!db_) return;
    std::size_t shard = KeyShard(key);
    std::lock_guard<std::mutex> lk(meta_shards_[shard].mu);
    rocksdb::WriteBatch batch;
    batch.Delete(cf_state_, rocksdb::Slice(key.data(), key.size()));
    batch.Delete(cf_meta_,  rocksdb::Slice(key.data(), key.size()));
    rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
    if (!s.ok()) throw std::runtime_error("RocksDB Delete failed: " + s.ToString());
}

bool RocksDBBackingStore::AppendEvent(std::string_view player_id, std::string_view event_blob) {
    if (!db_) return false;
    // 事件 key = player_id|seq; seq 取自 meta 的 event counter (与版本共用分桶锁)。
    std::size_t shard = KeyShard(player_id);
    std::lock_guard<std::mutex> lk(meta_shards_[shard].mu);
    std::string counter_key = std::string(player_id) + "|evt_seq";
    std::string craw;
    (void)db_->Get(rocksdb::ReadOptions(), cf_meta_, counter_key, &craw);
    uint64_t seq = DecodeVersion(craw) + 1;
    std::string evt_key = std::string(player_id) + "|" + std::to_string(seq);
    rocksdb::WriteBatch batch;
    batch.Put(cf_events_, evt_key, rocksdb::Slice(event_blob.data(), event_blob.size()));
    batch.Put(cf_meta_,  counter_key, EncodeVersion(seq));
    return db_->Write(rocksdb::WriteOptions(), &batch).ok();
}

bool RocksDBBackingStore::RebuildFromEvents(std::string_view player_id) {
    if (!db_) return false;
    // [PROTOTYPE] 遍历 kCfEvents 中 player_id 前缀的事件, 由调用方 apply 后写回 kCfState。
    // 此处仅做前缀扫描骨架, 真实重放语义需玩法层注入 apply 回调 (后续迭代)。
    // 注意: ReadOptions::iterate_lower_bound 是 const Slice*, 需 Slice 存活期覆盖迭代器,
    // 故用局部 Slice 变量而非临时 (避免悬垂引用)。
    const std::string seek_key = std::string(player_id) + "|";
    const rocksdb::Slice prefix_slice(seek_key);
    rocksdb::ReadOptions ro;
    ro.iterate_lower_bound = &prefix_slice;
    rocksdb::Iterator* it = db_->NewIterator(ro, cf_events_);
    if (!it) return false;
    for (it->Seek(seek_key); it->Valid() && it->key().starts_with(prefix_slice); it->Next()) {
        // 事件逐条存在; 真实重放在此调用 apply(it->value()) 并 Put 到 cf_state。
        // 当前仅确认可遍历, 不修改 state (自修复流程在后续 W4 迭代补全)。
    }
    delete it;
    return true;
}

}  // namespace rocksdb_proxy
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
