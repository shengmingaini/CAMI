#pragma once
// ============================================================================
// game/content/config_manager.h — 配置懒加载 + LRU 缓存 (纯 STL, 零外部依赖)
// ----------------------------------------------------------------------------
// 启动只加载 Tier 0 模块的 ConfigSet; 模块激活时按需解析 data/configs/*.json。
// 缓存带 LRU + 容量上限 (默认 64MB) + 引用计数语义 (模块卸载可清理)。
// ConfigSource 抽象: MODULES=ON 接 protobuf json_format (data/configs/*.json),
// OFF 构建用桩实现 (仅内存计数, 便于单测)。
// ============================================================================
#include <cstddef>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace cami {
namespace game {
namespace content {

// 配置源抽象: 负责"读取 + 反序列化"一个 ConfigSet。
// - [PROD] MODULES=ON: protobuf json_format.Parse (与 verify/load_configs.py 对齐)
// - [STUB] OFF 构建/单测: 内存桩 (返回占位数据)
class ConfigSource {
public:
    virtual ~ConfigSource() = default;
    // 加载配置集, 成功返回 true; out 内为已解析的字节大小 (用于 LRU 计费)
    virtual bool Load(const std::string& config_set, std::size_t& bytes_out) = 0;
};

class ConfigManager {
public:
    explicit ConfigManager(std::size_t capacity_bytes = 64u * 1024u * 1024u);

    // 确保某 ConfigSet 已加载 (幂等)。失败返回 false 并写 err。
    bool EnsureLoaded(const std::string& config_set, std::string* err = nullptr);
    bool IsLoaded(const std::string& config_set) const;

    // 缓存状态
    std::size_t CacheBytes() const;     // 当前占用
    std::size_t Capacity() const;       // 容量上限
    std::size_t CacheEntries() const;   // 缓存条目数

    // 手动淘汰 (模块卸载时清理)
    bool Evict(const std::string& config_set);

    // 接线
    void SetSource(ConfigSource* src);
    void SetCapacity(std::size_t capacity_bytes);

private:
    void Touch(const std::string& key);
    void EvictIfNeeded();

    struct Entry { std::string key; std::size_t bytes; };
    std::list<Entry> lru_;                                   // front=最近使用
    std::unordered_map<std::string, std::list<Entry>::iterator> index_;
    std::unordered_map<std::string, bool> loaded_;           // 已加载标记 (幂等)
    std::size_t capacity_;
    std::size_t bytes_ = 0;
    ConfigSource* src_ = nullptr;
};

}  // namespace content
}  // namespace game
}  // namespace cami
