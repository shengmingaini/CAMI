#include "game/content/config_manager.h"

#include <utility>

namespace cami {
namespace game {
namespace content {

ConfigManager::ConfigManager(std::size_t capacity_bytes)
    : capacity_(capacity_bytes) {}

void ConfigManager::SetCapacity(std::size_t capacity_bytes) {
    capacity_ = capacity_bytes;
    EvictIfNeeded();
}

void ConfigManager::SetSource(ConfigSource* src) { src_ = src; }

bool ConfigManager::EnsureLoaded(const std::string& config_set, std::string* err) {
    auto it = loaded_.find(config_set);
    if (it != loaded_.end()) {
        Touch(config_set);  // 已加载: 刷新 LRU 位置, 幂等返回
        return true;
    }
    if (!src_) {
        if (err) *err = "config source not set";
        return false;
    }
    std::size_t bytes = 0;
    if (!src_->Load(config_set, bytes)) {
        if (err) *err = "config source failed to load '" + config_set + "'";
        return false;
    }
    // 入 LRU
    lru_.push_front(Entry{config_set, bytes});
    index_[config_set] = lru_.begin();
    bytes_ += bytes;
    loaded_[config_set] = true;
    EvictIfNeeded();
    return true;
}

bool ConfigManager::IsLoaded(const std::string& config_set) const {
    return loaded_.find(config_set) != loaded_.end();
}

std::size_t ConfigManager::CacheBytes() const { return bytes_; }
std::size_t ConfigManager::Capacity() const   { return capacity_; }
std::size_t ConfigManager::CacheEntries() const { return lru_.size(); }

bool ConfigManager::Evict(const std::string& config_set) {
    auto it = index_.find(config_set);
    if (it == index_.end()) return false;
    bytes_ -= it->second->bytes;
    lru_.erase(it->second);
    index_.erase(it);
    loaded_.erase(config_set);
    return true;
}

void ConfigManager::Touch(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return;
    lru_.splice(lru_.begin(), lru_, it->second);  // 移到 front (最近使用)
}

void ConfigManager::EvictIfNeeded() {
    // LRU 淘汰最久未用, 直到低于容量
    while (!lru_.empty() && bytes_ > capacity_) {
        const std::string victim = lru_.back().key;
        (void)Evict(victim);
    }
}

}  // namespace content
}  // namespace game
}  // namespace cami
