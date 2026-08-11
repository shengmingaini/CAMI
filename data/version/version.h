#pragma once
// ============================================================================
// data/version/version.h — 版本校验模块 (Week4 D5)
// ----------------------------------------------------------------------------
// 乐观锁 (Optimistic Locking) / CAS: 防多节点并发覆盖玩家数据 (架构 §5.3)
//   模型 DB 行: UPDATE t SET value=?, version=version+1
//                WHERE player_id=? AND version=<expected>
//   仅当 affected_rows=1 时写入成功; 并发冲突者 affected_rows=0 -> 被拦截。
// [PRODUCTION] 逻辑与具体存储无关; DB 接入由 sync 模块落库时套用同款 CAS。
// ============================================================================
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cami {
namespace data {
namespace version {

// 版本化行 (value + 单调 version)
struct VersionedRow {
    std::string value;
    uint64_t version = 0;
};

// 线程安全版本化存储, 提供 CAS 更新
class VersionedStore {
public:
    // 读取 (返回快照)
    std::optional<VersionedRow> Load(std::string_view key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = rows_.find(std::string(key));
        if (it == rows_.end()) return std::nullopt;
        return it->second;
    }

    // 初始化一行 (首次创建, version=0)
    void Init(std::string_view key, std::string value) {
        std::lock_guard<std::mutex> lk(mu_);
        rows_.emplace(std::string(key), VersionedRow{std::move(value), 0});
    }

    // CAS: 仅当当前 version == expected 时写入新值并 version+1
    // 返回 true=成功(等价于 DB affected_rows=1), false=版本冲突被拦截
    bool Cas(std::string_view key, std::string_view new_value, uint64_t expected_version) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = rows_.find(std::string(key));
        if (it == rows_.end()) return false;
        if (it->second.version != expected_version) return false;  // 冲突拦截
        it->second.value = std::string(new_value);
        it->second.version += 1;
        return true;
    }

    // 无条件写入并 version+1 (Put 语义; 与 Cas 不同, 不校验当前版本)。
    // 返回写入后的版本号。Data Service 的 Put (非并发安全语义) 用它,
    // 避免"缓存已写但版本 Cas 静默失败"造成缓存/版本不一致。
    uint64_t Set(std::string_view key, std::string value) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = rows_.find(std::string(key));
        if (it == rows_.end()) {
            rows_.emplace(std::string(key), VersionedRow{std::move(value), 0});
            return 1;
        }
        it->second.value = std::move(value);
        it->second.version += 1;
        return it->second.version;
    }

    // D6 方案 B: 显式版本写 (本地版本 = DB 已知版本)。
    // 本地版本仅是"最近已知"缓存, 权威在 MySQL player_state.version。
    void UpsertVersion(std::string_view key, std::string value, uint64_t version) {
        std::lock_guard<std::mutex> lk(mu_);
        rows_[std::string(key)] = VersionedRow{std::move(value), version};
    }

    // 删除本地版本记录 (Delete 双删后清理)
    void Erase(std::string_view key) {
        std::lock_guard<std::mutex> lk(mu_);
        rows_.erase(std::string(key));
    }

    // 批量 CAS (sync 落库): 返回成功落库条数
    // changes: (key,new_value); expected: key->期望版本
    std::size_t BatchCas(const std::vector<std::pair<std::string, std::string>>& changes,
                         const std::unordered_map<std::string, uint64_t>& expected) {
        std::size_t ok = 0;
        for (auto& kv : changes) {
            uint64_t exp = 0;
            auto eit = expected.find(kv.first);
            if (eit != expected.end()) exp = eit->second;
            if (Cas(kv.first, kv.second, exp)) ++ok;
        }
        return ok;
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return rows_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, VersionedRow> rows_;
};

}  // namespace version
}  // namespace data
}  // namespace cami
