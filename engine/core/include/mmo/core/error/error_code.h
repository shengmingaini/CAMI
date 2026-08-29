#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace mmo::core {

/// 全局统一错误码枚举（int16_t，值固定，序列化后跨进程稳定）。
/// 禁止任何模块自行定义第二套顶层 ErrorCode 枚举。
/// 需要扩展错误种类时，使用 Error.domain 细分，而非新增顶层码。
enum class ErrorCode : int16_t {
    OK = 0,
    INVALID_ARGUMENT = 1,
    NOT_FOUND = 2,
    TIMEOUT = 3,
    BUSY = 4,
    VERSION_CONFLICT = 5,
    UNAUTHORIZED = 6,
    RATE_LIMITED = 7,
    INTERNAL_ERROR = 8,
};

/// 数值 -> 名称，与枚举值稳定对应。未知值返回 "UNKNOWN"。
const char* ToString(ErrorCode code) noexcept;

/// 名称 -> 数值；未知名称或越界数值返回 nullopt（禁止崩溃）。
/// 也接受纯数字字符串（如 "0"），越界（> INTERNAL_ERROR）同样返回 nullopt。
std::optional<ErrorCode> FromString(std::string_view name) noexcept;

/// 可重试错误：网络类瞬时失败（TIMEOUT / BUSY / RATE_LIMITED）。
bool IsRetryable(ErrorCode code) noexcept;

/// 受控错误域集合：Error.domain 只允许取这些值。
/// 扩展系统请复用已有域或经架构评审新增，禁止私有域污染。
namespace domain {
inline constexpr std::string_view kCore    = "core";
inline constexpr std::string_view kNet     = "net";
inline constexpr std::string_view kScene   = "scene";
inline constexpr std::string_view kCombat  = "combat";
inline constexpr std::string_view kData    = "data";
inline constexpr std::string_view kEconomy = "economy";
inline constexpr std::string_view kLua     = "lua";
}  // namespace domain

/// 校验 domain 是否属于受控集合（用于测试 / 断言；构造不强制，保证透传语义）。
bool IsValidDomain(std::string_view dom) noexcept;

}  // namespace mmo::core
