// server/gamenode/combat/src/damage/damage_formula.cpp — TASK-022 §15.2 / §20.1
//
// 配置加载：走 core::ConfigManager（TASK-003），与 TASK-016 exp_curve 同套路。
// 目的有两点：
//   ① 文件读取留在 core，本模块 src/ 内不出现任何文件流类型（§24 静态红线扫描）；
//   ② 复用已验收的 JSON 解析与快照替换，不新增第二套解析器。
//
// 红线：本文件不得引入任何外部中间件依赖，也不得有任何文件读写（§21 / §24）。
//       配置读取只在启动期/测试期发生，**绝不在伤害结算路径上**（§10 热路径）。
//       注：注释正文同样受 §24 文本扫描约束，故此处不原样列出被禁词。

#include "mmo/game/combat/damage/damage_formula.h"

#include <string>
#include <string_view>

#include "mmo/core/config/config_manager.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"

namespace mmo::game::combat {
namespace {

constexpr std::string_view kDomain = "combat";
constexpr std::string_view kPrefix = "damage.";

core::Error Fail(std::string_view msg) {
    return core::Error(core::ErrorCode::INVALID_ARGUMENT, msg, kDomain);
}

/// 拼 `damage.<field>` 键。返回 std::string（ConfigManager::Get 收 string_view）。
std::string Key(std::string_view field) { return std::string(kPrefix) + std::string(field); }

/// 读 int64 字段；缺失或类型不符 → Fail（禁止默认值静默生成）。
core::Result<std::int64_t> NeedInt(std::string_view field) {
    const std::string key = Key(field);
    auto v = core::ConfigManager::Get<std::int64_t>(key);
    if (!v.HasValue()) {
        return core::Result<std::int64_t>::Fail(
            core::Error(v.Err().Code(), "damage formula missing int field: " + key, kDomain));
    }
    return v;
}

/// 读 bool 字段。
core::Result<bool> NeedBool(std::string_view field) {
    const std::string key = Key(field);
    auto v = core::ConfigManager::Get<bool>(key);
    if (!v.HasValue()) {
        return core::Result<bool>::Fail(
            core::Error(v.Err().Code(), "damage formula missing bool field: " + key, kDomain));
    }
    return v;
}

}  // namespace

/// 已加载的公式文件路径（幂等用）。
///
/// **为什么必须按路径判断**：ConfigManager 是**进程级**配置中心，只凭「键是否存在」
/// 判断"已加载"会给出错误答案——用另一个路径（哪怕是根本不存在的文件）再次调用
/// LoadFromFile 时，键还在，于是直接跳过 LoadFile 并返回上一份配置，
/// 造成「加载了错误路径却报告成功」的静默事故（单测 `test_formula_config` 抓到）。
std::string& LoadedPath() {
    static std::string g;
    return g;
}

core::Result<DamageFormula> DamageFormula::LoadFromFile(std::string_view path) noexcept {
    // 同一个路径重复加载 → 跳过（ConfigManager 键冲突会返回 INVALID_ARGUMENT）；
    // 不同路径 → 必须真的 LoadFile，失败就如实失败，绝不拿旧配置顶替。
    if (LoadedPath() != path) {
        const auto loaded = core::ConfigManager::LoadFile(path);
        if (!loaded.HasValue()) {
            return core::Result<DamageFormula>::Fail(
                core::Error(loaded.Err().Code(),
                            "damage formula load failed: " + std::string(path), kDomain));
        }
        LoadedPath() = std::string(path);
    }

    DamageFormula f{};
    struct IntField {
        std::string_view name;
        std::int64_t* dst;
    };
    const IntField ints[] = {
        {"rate_scale", &f.rate_scale},
        {"crit_damage_default", &f.crit_damage_default},
        {"crit_rate_max", &f.crit_rate_max},
        {"dodge_rate_base", &f.dodge_rate_base},
        {"dodge_per_agility", &f.dodge_per_agility},
        {"dodge_rate_max", &f.dodge_rate_max},
        {"mitigation_k", &f.mitigation_k},
        {"mitigation_max", &f.mitigation_max},
        {"min_damage", &f.min_damage},
        {"max_raw_damage", &f.max_raw_damage},
        {"lethal_hp_threshold", &f.lethal_hp_threshold},
        {"log_samples_per_second", &f.log_samples_per_second},
        {"log_ring_capacity", &f.log_ring_capacity},
        {"tick_rate_hz", &f.tick_rate_hz},
    };
    for (const auto& fld : ints) {
        auto v = NeedInt(fld.name);
        if (!v.HasValue()) return core::Result<DamageFormula>::Fail(v.Err());
        *fld.dst = v.Value();
    }

    struct BoolField {
        std::string_view name;
        bool* dst;
    };
    BoolField bools[] = {
        {"true_damage_ignores_mitigation", &f.true_damage_ignores_mitigation},
        {"true_damage_ignores_dodge", &f.true_damage_ignores_dodge},
        {"true_damage_can_crit", &f.true_damage_can_crit},
        {"shield_absorbs_before_hp", &f.shield_absorbs_before_hp},
        {"heal_overheal_counts_to_stats", &f.heal_overheal_counts_to_stats},
    };
    for (const auto& fld : bools) {
        auto v = NeedBool(fld.name);
        if (!v.HasValue()) return core::Result<DamageFormula>::Fail(v.Err());
        *fld.dst = v.Value();
    }

    const auto ok = f.Validate();
    if (!ok.HasValue()) return core::Result<DamageFormula>::Fail(ok.Err());
    return core::Result<DamageFormula>::Ok(f);
}

core::Result<void> DamageFormula::Validate() const noexcept {
    if (rate_scale <= 0) return core::Result<void>::Fail(Fail("rate_scale must be > 0"));
    if (mitigation_k <= 0) return core::Result<void>::Fail(Fail("mitigation_k must be > 0"));
    if (mitigation_max < 0 || mitigation_max >= rate_scale) {
        return core::Result<void>::Fail(Fail("mitigation_max must be in [0, rate_scale)"));
    }
    if (crit_rate_max < 0 || crit_rate_max > rate_scale) {
        return core::Result<void>::Fail(Fail("crit_rate_max must be in [0, rate_scale]"));
    }
    if (dodge_rate_max < 0 || dodge_rate_max > rate_scale) {
        return core::Result<void>::Fail(Fail("dodge_rate_max must be in [0, rate_scale]"));
    }
    if (min_damage < 0) return core::Result<void>::Fail(Fail("min_damage must be >= 0"));
    if (max_raw_damage <= 0) return core::Result<void>::Fail(Fail("max_raw_damage must be > 0"));
    if (tick_rate_hz <= 0) return core::Result<void>::Fail(Fail("tick_rate_hz must be > 0"));
    if (log_samples_per_second < 0) {
        return core::Result<void>::Fail(Fail("log_samples_per_second must be >= 0"));
    }
    if (log_ring_capacity <= 0) {
        return core::Result<void>::Fail(Fail("log_ring_capacity must be > 0"));
    }
    if (crit_damage_default < 0) {
        return core::Result<void>::Fail(Fail("crit_damage_default must be >= 0"));
    }
    return core::Result<void>::Ok();
}

}  // namespace mmo::game::combat
