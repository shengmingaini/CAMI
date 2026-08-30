#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace mmo::core {

/// 日志级别。数值越小越详细；运行期判定统一用 `level >= min_level`。
///
/// 级别是 Logger 唯一的运行期开关维度：ShouldLog 只做一次 relaxed 原子读 + 比较，
/// 因此关闭日志时 MMO_LOG 的开销≈单次分支（见 docs/PERFORMANCE.md 实测）。
enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

/// 默认级别：Info。Trace / Debug 必须由配置显式开启（生产默认关闭）。
inline constexpr LogLevel kDefaultLogLevel = LogLevel::Info;

/// 级别 -> 固定名称（"TRACE".."FATAL"）；未知值返回 "UNKNOWN"。
/// 名称不含填充空格，列对齐由文本格式化器负责，避免污染解析。
const char* ToString(LogLevel level) noexcept;

/// 名称 -> 级别；大小写不敏感，未知名称返回 nullopt（禁止崩溃、禁止抛异常）。
///
/// 命名不复用 `FromString` 重载：ErrorCode 已有 `optional<ErrorCode> FromString(string_view)`，
/// 同签名仅返回类型不同的重载在调用点会产生歧义（重载解析不看返回类型），故取名 ParseLogLevel。
std::optional<LogLevel> ParseLogLevel(std::string_view name) noexcept;

}  // namespace mmo::core
