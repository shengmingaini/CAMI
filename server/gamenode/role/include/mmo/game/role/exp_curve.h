#pragma once

/// TASK-016 · 升级经验曲线（§8「配置化，禁止硬编码在代码里」/ §21）。
///
/// 曲线来自 `config/gameplay/exp_curve.json`：
///     exp_to_next(level) = base * pow(level, exponent)
/// 配置缺失或非法 → LoadFromFile 返回错误，**禁止用默认值静默启动**（§19）。

#include <cstdint>

#include "mmo/core/error/result.h"

namespace mmo::game::role {

struct ExpCurveConfig {
    std::uint32_t max_level{0};   // 满级（达到后不再升级）
    double base{0.0};             // 系数
    double exponent{0.0};         // 指数
};

class ExpCurve {
public:
    /// 只允许从配置构造（禁止默认构造后静默使用，§19）。
    ExpCurve() = delete;

    /// 从 JSON 配置文件加载。文件不存在 / 解析失败 / 数值非法 → Fail。
    static core::Result<ExpCurve> LoadFromFile(const char* path);

    /// 从已校验的配置构造（测试用）。非法 → Fail。
    static core::Result<ExpCurve> FromConfig(const ExpCurveConfig& cfg);

    /// 升到下一级所需经验；已满级返回 0。
    std::uint64_t ExpToNext(std::uint32_t level) const noexcept;

    std::uint32_t MaxLevel() const noexcept { return cfg_.max_level; }
    const ExpCurveConfig& Config() const noexcept { return cfg_; }

private:
    explicit ExpCurve(const ExpCurveConfig& cfg) noexcept : cfg_(cfg) {}
    static bool Validate(const ExpCurveConfig& cfg) noexcept;

    ExpCurveConfig cfg_{};
};

}  // namespace mmo::game::role
