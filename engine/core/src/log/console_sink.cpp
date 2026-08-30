// engine/core/src/log/console_sink.cpp — TASK-002 控制台 sink（开发用）
//
// 红线遵守（§21）：不使用 std::cout / printf，统一走 stdout 的 FILE* + fwrite，
// 便于按整行批量写出，也避免与 iostream 的格式化开销。

#include "mmo/core/log/log_sink.h"

#include <cstdio>
#include <cstring>

#include "log/log_formatter.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mmo::core {
namespace {

const char* LevelColor(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "\x1b[90m";  // 亮黑
        case LogLevel::Debug:
            return "\x1b[36m";  // 青
        case LogLevel::Info:
            return "\x1b[32m";  // 绿
        case LogLevel::Warn:
            return "\x1b[33m";  // 黄
        case LogLevel::Error:
            return "\x1b[31m";  // 红
        case LogLevel::Fatal:
            return "\x1b[35m";  // 品红
    }
    return "\x1b[0m";
}

bool StdoutIsTerminal() noexcept {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

class ConsoleSink final : public ILogSink {
public:
    ConsoleSink(bool color, LogFormat format)
        // 重定向到文件 / 管道时自动关闭颜色，避免转义序列污染日志内容。
        : color_(color && StdoutIsTerminal()), format_(format) {
#ifdef _WIN32
        if (color_) {
            const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0) {
                // 未开启虚拟终端处理时，Windows 控制台会把转义序列当普通字符打印。
                (void)SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
#endif
    }

    void Write(const LogRecord& rec) noexcept override {
        char line[detail::kFormatLineCapacity + 1];
        const std::size_t n = (format_ == LogFormat::kJson)
                                  ? detail::FormatJsonLine(rec, line, sizeof(line) - 1)
                                  : detail::FormatTextLine(rec, line, sizeof(line) - 1);
        std::size_t len = n;
        if (len < sizeof(line) - 1) {
            line[len++] = '\n';
        }
        if (color_ && format_ == LogFormat::kText) {
            const char* color = LevelColor(rec.level);
            std::fwrite(color, 1, std::strlen(color), stdout);
            std::fwrite(line, 1, len, stdout);
            std::fwrite("\x1b[0m", 1, 4, stdout);
        } else {
            std::fwrite(line, 1, len, stdout);
        }
    }

    void Flush() noexcept override { std::fflush(stdout); }

private:
    bool color_;
    LogFormat format_;
};

}  // namespace

std::shared_ptr<ILogSink> MakeConsoleSink(bool color, LogFormat format) {
    return std::make_shared<ConsoleSink>(color, format);
}

}  // namespace mmo::core
