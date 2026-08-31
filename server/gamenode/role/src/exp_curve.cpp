/// TASK-016 · 升级经验曲线实现（配置化加载，§8「配置化，禁止硬编码在代码里」）。
///
/// 配置源：`config/gameplay/exp_curve.json`
///     {
///       "exp_curve": { "base": 100.0, "exponent": 1.5, "max_level": 60 }
///     }
/// 读取走 `core::ConfigManager`（TASK-003），键路径为点号形式（`exp_curve.base` 等），
/// 与项目其它配置（`tick.hz` 等）保持同一套机制，**不自带第二套 JSON 解析器**。
///
/// §19：文件不存在 / 键缺失 / 数值非法 → 返回错误，禁止回退默认值静默启动。

#include "mmo/game/role/exp_curve.h"

#include <cmath>
#include <cstdint>

#include "mmo/core/config/config_manager.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"

namespace mmo::game::role {
namespace {

constexpr const char* kKeyBase = "exp_curve.base";
constexpr const char* kKeyExponent = "exp_curve.exponent";
constexpr const char* kKeyMaxLevel = "exp_curve.max_level";

core::Result<ExpCurve> Fail(const char* message, core::ErrorCode code) {
    return core::Result<ExpCurve>::Fail(core::Error(code, message, core::domain::kCore));
}

}  // namespace

bool ExpCurve::Validate(const ExpCurveConfig& cfg) noexcept {
    if (cfg.max_level < 1u) return false;                    // 至少 1 级
    if (!std::isfinite(cfg.base) || !std::isfinite(cfg.exponent)) return false;
    if (!(cfg.base > 0.0)) return false;                      // 系数必须为正
    if (!(cfg.exponent > 0.0)) return false;                  // 指数必须为正
    // 满级所需经验必须能安全放进 uint64，否则 AddExp 的溢出检测失去意义。
    const double top = cfg.base * std::pow(static_cast<double>(cfg.max_level), cfg.exponent);
    if (!std::isfinite(top) || top > 1.0e18) return false;
    return true;
}

core::Result<ExpCurve> ExpCurve::FromConfig(const ExpCurveConfig& cfg) {
    if (!Validate(cfg)) {
        return Fail("invalid exp curve config", core::ErrorCode::INVALID_ARGUMENT);
    }
    return core::Result<ExpCurve>::Ok(ExpCurve(cfg));
}

core::Result<ExpCurve> ExpCurve::LoadFromFile(const char* path) {
    if (path == nullptr) {
        return Fail("null config path", core::ErrorCode::INVALID_ARGUMENT);
    }

    // ConfigManager 是**进程级**配置中心：LoadFile 把文件内容合并进全局快照。
    // 幂等处理：若本次加载失败只是因为「这些键已登记过」（重复 Load），
    // 则视为已加载并继续读值；否则（文件不存在 / JSON 非法）按 §19 报错。
    const bool already_loaded = core::ConfigManager::Contains(kKeyBase) &&
                                core::ConfigManager::Contains(kKeyExponent) &&
                                core::ConfigManager::Contains(kKeyMaxLevel);
    if (!already_loaded) {
        const auto loaded = core::ConfigManager::LoadFile(path);
        if (!loaded.HasValue()) {
            return Fail("exp curve config missing", core::ErrorCode::NOT_FOUND);
        }
    }

    const auto base = core::ConfigManager::Get<double>(kKeyBase);
    const auto exponent = core::ConfigManager::Get<double>(kKeyExponent);
    const auto max_level = core::ConfigManager::Get<std::uint32_t>(kKeyMaxLevel);
    if (!base.HasValue() || !exponent.HasValue() || !max_level.HasValue()) {
        return Fail("exp curve config missing key", core::ErrorCode::INVALID_ARGUMENT);
    }

    ExpCurveConfig cfg;
    cfg.base = base.Value();
    cfg.exponent = exponent.Value();
    cfg.max_level = max_level.Value();
    return FromConfig(cfg);
}

std::uint64_t ExpCurve::ExpToNext(std::uint32_t level) const noexcept {
    if (level >= cfg_.max_level) return 0;  // 满级：不再需要经验
    if (level == 0u) level = 1u;            // 0 级按 1 级算（防御）
    const double v = cfg_.base * std::pow(static_cast<double>(level), cfg_.exponent);
    if (!std::isfinite(v) || v <= 0.0) return 1;
    return static_cast<std::uint64_t>(v);
}

}  // namespace mmo::game::role
