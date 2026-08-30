// engine/core/src/log/log_level.cpp — TASK-002 日志级别名称映射

#include "mmo/core/log/log_level.h"

#include <cstddef>

namespace mmo::core {
namespace {

/// 大小写不敏感比较（仅 ASCII，日志级别名称为常量集，不涉及本地化）。
bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = a[i];
        const char cb = b[i];
        if (ca == cb) {
            continue;
        }
        const bool upper_match = (ca >= 'A' && ca <= 'Z' && cb >= 'a' && cb <= 'z' &&
                                  (ca - 'A') == (cb - 'a'));
        const bool lower_match = (ca >= 'a' && ca <= 'z' && cb >= 'A' && cb <= 'Z' &&
                                  (cb - 'A') == (ca - 'a'));
        if (!upper_match && !lower_match) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* ToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
    }
    return "UNKNOWN";
}

std::optional<LogLevel> ParseLogLevel(std::string_view name) noexcept {
    // 主名 + 常用别名，便于运维在配置里写 warning / err / critical。
    if (EqualsIgnoreCase(name, "TRACE")) return LogLevel::Trace;
    if (EqualsIgnoreCase(name, "DEBUG")) return LogLevel::Debug;
    if (EqualsIgnoreCase(name, "INFO")) return LogLevel::Info;
    if (EqualsIgnoreCase(name, "WARN") || EqualsIgnoreCase(name, "WARNING")) return LogLevel::Warn;
    if (EqualsIgnoreCase(name, "ERROR") || EqualsIgnoreCase(name, "ERR")) return LogLevel::Error;
    if (EqualsIgnoreCase(name, "FATAL") || EqualsIgnoreCase(name, "CRITICAL")) return LogLevel::Fatal;
    return std::nullopt;
}

}  // namespace mmo::core
