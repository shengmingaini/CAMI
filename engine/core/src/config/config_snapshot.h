#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config/json_parser.h"
#include "mmo/core/error/result.h"

namespace mmo::core::detail {

/// 透明哈希：允许用 string_view 直接查 unordered_map<string, ...>，
/// 省掉每次 Get 构造临时 std::string 的开销（读路径禁止分配）。
struct TransparentStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
    std::size_t operator()(const std::string& text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
};

using ConfigValueMap =
    std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>>;

/// ConfigSnapshot —— 不可变配置快照。
///
/// State Owner  : 快照一旦发布即不可变；写入权归 ControlService / 运维线程（冷路径），
///                运行期所有业务线程只读（PROJECT_REQUIREMENTS §10 / §12）。
/// Thread Safety: 只读并发安全；写入只允许发生在「发布之前」的构建阶段。
/// Hot Path     : Find 位于热路径读侧，必须做到无锁、无分配。
///
/// 禁止：发布后就地修改；把裸指针/引用交给调用方长期持有（只能由 ConfigManager 短时持有）。
class ConfigSnapshot {
public:
    ConfigSnapshot() = default;

    std::uint64_t Version() const noexcept { return version_; }
    std::size_t Size() const noexcept { return values_.size(); }

    /// 插入或覆盖一个键（仅在发布前的构建阶段调用）。
    void Insert(std::string key, std::string value) {
        if (values_.find(key) == values_.end()) {
            order_.push_back(key);
        }
        values_[std::move(key)] = std::move(value);
    }

    /// 查找，返回快照内的稳定字符串指针；不存在返回 nullptr。
    /// 指针有效期 = 本快照的生命期，由调用方（ConfigManager 的线程局部缓存）保证。
    const std::string* Find(std::string_view key) const noexcept {
        const auto it = values_.find(key);
        return (it == values_.end()) ? nullptr : &(it->second);
    }

    bool Contains(std::string_view key) const noexcept { return Find(key) != nullptr; }

    /// 把另一个快照的全部键值合并进来（同键覆盖）。只在发布前的构建阶段调用。
    void MergeFrom(const ConfigSnapshot& other) {
        // 按插入序遍历，保证合并后 Keys() 顺序稳定可预期。
        for (const std::string& key : other.order_) {
            const auto it = other.values_.find(key);
            if (it != other.values_.end()) {
                Insert(key, it->second);
            }
        }
    }

    /// 稳定顺序的键列表（插入顺序），便于运维核对与测试断言。
    std::vector<std::string> Keys() const { return order_; }

    /// 复制一份可写副本（用于 Set 的写时复制）。
    std::shared_ptr<ConfigSnapshot> Cloned() const {
        auto copy = std::make_shared<ConfigSnapshot>();
        copy->values_ = values_;
        copy->order_ = order_;
        return copy;
    }

    /// 从扁平化 JSON 结果构建快照。
    ///
    /// 冲突语义：同一 key 在同一批里出现两次（例如两个配置文件都定义了 tick.hz）
    /// 直接返回 INVALID_ARGUMENT —— 宁可加载失败，也不让「后加载者静默胜出」。
    /// version 由调用方发布时写入。
    static Result<std::shared_ptr<ConfigSnapshot>> Build(const config_internal::FlatMap& flat) {
        auto snap = std::make_shared<ConfigSnapshot>();
        for (const auto& entry : flat) {
            if (snap->values_.find(entry.first) != snap->values_.end()) {
                std::string message = "duplicate key: ";
                message += entry.first;
                return Result<std::shared_ptr<ConfigSnapshot>>::Fail(
                    Error(ErrorCode::INVALID_ARGUMENT, message));
            }
            snap->Insert(entry.first, entry.second);
        }
        return Result<std::shared_ptr<ConfigSnapshot>>::Ok(std::move(snap));
    }

    void SetVersion(std::uint64_t version) noexcept { version_ = version; }

private:
    ConfigValueMap values_;
    std::vector<std::string> order_;
    std::uint64_t version_ = 0;
};

}  // namespace mmo::core::detail
