#pragma once

/// TASK-029 · 价格表（§15.3 / §20.6：价格**必须配置化**，禁止代码硬编码价格）。
///
/// 配置文件：`config/gameplay/economy/prices.json`
/// ```json
/// { "version": 1,
///   "prices": [ { "item_id": 1001, "currency": 1, "unit_price": 50,
///                 "min_count": 1, "max_count": 99 } ] }
/// ```
/// 解析使用与 skill / quest / npc 配置同款的模块内极简 JSON 解析器（core 的
/// `ParseJson` 属 `config_internal`，不对外暴露，故各模块自建，见 price_table.cpp）。
///
/// 经济系统**不是热路径**（§10 = NO），因此此处直接用 unordered_map，
/// 不引入扁平表复杂度；单次查询 <100ns，远低于 §22 的 2us 命令预算。

#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "mmo/core/error/result.h"
#include "mmo/game/economy/currency.h"
#include "mmo/game/inventory/item.h"

namespace mmo::game::economy {

/// 单个物品的定价。
struct PriceEntry {
    CurrencyType currency{kCurrencyGold};
    std::int64_t unit_price{0};
    std::uint32_t min_count{1};
    std::uint32_t max_count{0};  // 0 = 不限

    /// 校验购买数量是否在 [min_count, max_count] 内。
    bool Allows(std::uint32_t count) const noexcept;

    /// 总价（unit_price * count）。溢出时返回 false（禁止静默截断）。
    bool Total(std::uint32_t count, std::int64_t& out) const noexcept;
};

/// 价格表（值语义，可被 SetPriceTable 拷贝进 EconomySystem）。
class PriceTable {
public:
    /// 从 JSON 文本加载（原子：解析失败时**不修改**既有内容）。
    core::Result<void> LoadFromText(std::string_view json) noexcept;

    /// 从文件加载（走 core::ConfigManager::ReadFile，模块内禁止文件流，§21）。
    core::Result<void> LoadFromFile(std::string_view path) noexcept;

    const PriceEntry* Find(inventory::ItemId id) const noexcept;

    std::size_t size() const noexcept { return entries_.size(); }
    void Clear() noexcept { entries_.clear(); ++version_; }
    std::uint32_t version() const noexcept { return version_; }

private:
    std::unordered_map<inventory::ItemId, PriceEntry> entries_;
    std::uint32_t version_{0};
};

}  // namespace mmo::game::economy
