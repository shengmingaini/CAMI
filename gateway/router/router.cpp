// gateway/router/router.cpp
// 一致性哈希路由：每 key 选后端节点，节点增减仅迁移 ~1/N 的 key。
//
// 设计约定：
// - 纯内存、零外部依赖、transport-agnostic；网关在需要定位后端（game 实例）时调用 route(key)。
// - 哈希用 FNV-1a 64-bit（跨平台稳定，不依赖 std::hash 实现差异），保证不同进程/平台同 key 同节点。
// - 虚拟节点（默认 100）使负载均衡、迁移平滑；reload_backends 为热更新入口（单线程原型直接重建环，
//   多线程场景应改为 atomic<shared_ptr<HashRing>> 交换，见文档开放问题）。
#include "gateway/router/router.h"

#include <algorithm>

namespace cami::gateway::router {

// === HashRing ===
HashRing::HashRing(int virtual_nodes_per_node) : vnodes_(virtual_nodes_per_node) {}

uint64_t HashRing::hash_fn(const std::string& s) const {
    // FNV-1a 64-bit 作基底 + fmix64 终段混淆（MurmurHash3）增强雪崩。
    // 仅靠 FNV-1a 对短结构化字符串（"nodeX#i"）雪崩不足，虚拟节点会在环上聚类 ->
    // 负载倾斜、迁移率偏离 1/N。fmix64 终段提供强扩散，保证 vnode 均匀散布。
    uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;            // FNV prime
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

void HashRing::add_node(const std::string& node_id) {
    for (int i = 0; i < vnodes_; ++i) {
        const std::string vkey = node_id + "#" + std::to_string(i);
        ring_.emplace(hash_fn(vkey), node_id);
    }
    node_vcount_[node_id] = vnodes_;
}

void HashRing::remove_node(const std::string& node_id) {
    for (int i = 0; i < vnodes_; ++i) {
        const std::string vkey = node_id + "#" + std::to_string(i);
        auto it = ring_.find(hash_fn(vkey));
        if (it != ring_.end() && it->second == node_id) ring_.erase(it);
    }
    node_vcount_.erase(node_id);
}

void HashRing::set_nodes(const std::vector<std::string>& nodes) {
    ring_.clear();
    node_vcount_.clear();
    for (const auto& n : nodes) add_node(n);
}

std::string HashRing::get_node(const std::string& key) const {
    if (ring_.empty()) return "";
    const uint64_t h = hash_fn(key);
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) it = ring_.begin();  // 环回
    return it->second;
}

// === Router ===
Router::Router(const RouterConfig& cfg) : cfg_(cfg), ring_(cfg.virtual_nodes_per_node) {}

std::string Router::route(const std::string& key) const { return ring_.get_node(key); }

void Router::add_backend(const std::string& id) { ring_.add_node(id); }
void Router::remove_backend(const std::string& id) { ring_.remove_node(id); }
void Router::reload_backends(const std::vector<std::string>& ids) { ring_.set_nodes(ids); }
std::size_t Router::backend_count() const { return ring_.node_count(); }

double Router::migration_ratio(const std::vector<std::string>& keys,
                                const std::vector<std::string>& new_backends) const {
    if (keys.empty()) return 0.0;
    HashRing candidate(cfg_.virtual_nodes_per_node);
    for (const auto& n : new_backends) candidate.add_node(n);
    std::size_t moved = 0;
    for (const auto& k : keys) {
        if (ring_.get_node(k) != candidate.get_node(k)) ++moved;
    }
    return static_cast<double>(moved) / static_cast<double>(keys.size());
}

} // namespace cami::gateway::router
