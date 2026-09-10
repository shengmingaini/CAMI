// server/dataservice/include/mmo/data/icache.h
//
// ICache —— 易失缓存层接口（TASK-026）。
//
// 对应外部键值缓存角色：提供低延迟的 Get/Put 与按前缀失效。
// 本任务只要求内存实现（InMemoryCache，有界 LRU + TTL）。
// 接口只管键值、TTL 与失效，不绑定任何具体缓存技术。

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/data/record.h"

namespace mmo::data {

/// 易失缓存层接口（对应外部键值缓存角色）。
class ICache {
public:
    virtual ~ICache() = default;

    /// 命中返回记录；未命中/过期返回 std::nullopt（Ok 包裹）。
    virtual core::Result<std::optional<Record>> Get(const DataKey& key) = 0;

    /// 写入（可带 TTL；ttl<=0 表示永不过期）。
    virtual core::Result<void> Put(const Record& rec, mmo::core::DurationMs ttl = {}) = 0;

    /// 失效单键。
    virtual core::Result<void> Invalidate(const DataKey& key) = 0;

    /// 失效匹配前缀的所有键（如 "inventory:"）。
    virtual core::Result<void> InvalidatePrefix(std::string_view prefix) = 0;
};

}  // namespace mmo::data
