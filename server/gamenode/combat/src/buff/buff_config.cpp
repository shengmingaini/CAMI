/// TASK-023 · Buff 配置加载（§21 配置化 / §24 红线）。
///
/// 全部 Buff 定义来自 `config/gameplay/buffs/*.json`（进程级 ConfigManager，键路径 `buffs[i].*`）。
/// 文件读取委托给 core 层 `ConfigManager::LoadFile`，本文件不直接做文件 IO（见红线 §24）。
/// 只依赖 core / role 的公开头，不 include 任何 `src/`。

#include "mmo/game/combat/buff/buff_system.h"

#include <cstdint>
#include <string>

#include "mmo/core/config/config_manager.h"
#include "mmo/game/role/attribute.h"

namespace mmo::game::combat::buff {

namespace {
using core::ConfigManager;

inline bool Has(const std::string& k) { return ConfigManager::Contains(k); }

template <typename T>
T GetOr(std::string_view key, T def) {
    const auto r = ConfigManager::Get<T>(key);
    return r.HasValue() ? r.Value() : def;
}

}  // namespace

core::Result<void> BuffRegistry::LoadFromConfig(std::string_view file) {
    // 幂等：同一文件重复加载不再触发键冲突（ConfigManager 合并会拒重复键）。
    if (!loaded_path_.empty() && loaded_path_ == file) {
        return core::Result<void>::Ok();
    }
    const auto loaded = ConfigManager::LoadFile(file);
    if (!loaded.HasValue()) {
        return core::Result<void>::Fail(loaded.Err());
    }
    loaded_path_ = std::string(file);

    for (int i = 0;; ++i) {
        const std::string base = "buffs[" + std::to_string(i) + "].";
        if (!Has(base + "id")) break;  // 数组结束

        BuffDef d;
        d.id = GetOr<std::uint32_t>(base + "id", 0);
        if (Has(base + "name")) d.name = ConfigManager::Get<std::string>(base + "name").Value();
        d.kind = static_cast<BuffKind>(GetOr<std::int64_t>(base + "kind", 0));
        d.stack_rule = static_cast<StackRule>(GetOr<std::int64_t>(base + "stack_rule", 1));
        d.max_stacks = static_cast<std::uint16_t>(GetOr<std::uint32_t>(base + "max_stacks", 1));
        d.duration_ms = GetOr<std::uint32_t>(base + "duration_ms", 0);
        d.tick_interval_ms = GetOr<std::uint32_t>(base + "tick_interval_ms", 0);
        d.shield_value = GetOr<std::int64_t>(base + "shield", 0);
        d.dispellable = !(Has(base + "dispellable") &&
                          ConfigManager::Get<std::int64_t>(base + "dispellable").Value() == 0);
        d.control_mask = static_cast<std::uint8_t>(GetOr<std::int64_t>(base + "control", 0));

        // 主属性 / 派生属性的加法 + 乘法贡献（键缺失则 0 / 1.0）
        for (std::size_t a = 0; a < role::kAttrCount; ++a) {
            const char* nm = role::AttrName(static_cast<role::AttrType>(a));
            const std::string mk = base + "modifiers." + nm;
            if (Has(mk)) d.attr_modifiers[a] = ConfigManager::Get<std::int64_t>(mk).Value();
            const std::string pk = base + "multipliers." + nm;
            if (Has(pk)) d.attr_multipliers[a] = ConfigManager::Get<double>(pk).Value();
        }

        // 周期效果（DOT/HOT）
        if (Has(base + "tick.amount"))
            d.tick_effect.amount = ConfigManager::Get<std::int64_t>(base + "tick.amount").Value();
        if (Has(base + "tick.school"))
            d.tick_effect.school =
                static_cast<std::uint8_t>(ConfigManager::Get<std::int64_t>(base + "tick.school").Value());
        if (Has(base + "tick.can_crit"))
            d.tick_effect.can_crit = ConfigManager::Get<std::int64_t>(base + "tick.can_crit").Value() != 0;
        if (Has(base + "tick.is_heal"))
            d.tick_effect.is_heal = ConfigManager::Get<std::int64_t>(base + "tick.is_heal").Value() != 0;

        const auto added = Add(std::move(d));
        if (!added.HasValue()) return core::Result<void>::Fail(added.Err());
    }
    return core::Result<void>::Ok();
}

}  // namespace mmo::game::combat::buff
