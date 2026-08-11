#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "gateway/router/router_types.h"

namespace cami::gateway::router {

// 一致性哈希环：物理节点 → 多个虚拟节点均匀散布在 [0, 2^64) 环上。
// 路由：对 key 哈希，顺时针找第一个虚拟节点对应的物理节点。
// 节点增减仅迁移 ~1/N 的 key（N=节点数），满足"迁移 < 10%"验收（N>=11 时约 9%）。
class HashRing {
public:
    explicit HashRing(int virtual_nodes_per_node = 100);
    void add_node(const std::string& node_id);
    void remove_node(const std::string& node_id);
    void set_nodes(const std::vector<std::string>& nodes);  // 热更新：替换整环
    std::string get_node(const std::string& key) const;     // 空串 = 无节点
    std::size_t node_count() const { return node_vcount_.size(); }

private:
    uint64_t hash_fn(const std::string& s) const;  // FNV-1a 64，跨平台稳定
    int vnodes_;
    std::map<uint64_t, std::string> ring_;                  // hash -> 物理节点 id
    std::unordered_map<std::string, int> node_vcount_;
};

// 路由门面：一致性哈希选后端 + 热更新 + 迁移率预估（验收证据）。
class Router {
public:
    explicit Router(const RouterConfig& cfg);
    std::string route(const std::string& key) const;        // 返回后端 id，空串 = 无可用
    void add_backend(const std::string& id);
    void remove_backend(const std::string& id);
    void reload_backends(const std::vector<std::string>& ids);  // 热更新：替换环
    std::size_t backend_count() const;

    // 迁移率预估：在 keys 集合上对比"切换为 new_backends 后"的 route 变化比例。
    // 用于非侵入式验证 节点增减迁移 key < 10%。
    double migration_ratio(const std::vector<std::string>& keys,
                           const std::vector<std::string>& new_backends) const;

private:
    RouterConfig cfg_;
    HashRing ring_;
};

} // namespace cami::gateway::router
