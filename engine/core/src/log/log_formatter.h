#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "mmo/core/log/log_record.h"

namespace mmo::core::detail {

/// 单行输出缓冲容量：JSON 需转义，按 message 的两倍预留。
inline constexpr std::size_t kFormatLineCapacity = kMaxLogMessage * 2 + 256;

/// 固定容量写入器：只写入调用方提供的缓冲，绝不分配、绝不溢出。
/// 容量不足时静默截断（日志截断优于崩溃，也优于在日志路径上堆分配）。
class LineWriter final {
public:
    LineWriter(char* buf, std::size_t cap) noexcept : buf_(buf), cap_(cap) {}

    /// 追加运行时字符串（按 string_view 的长度）。
    void Append(std::string_view s) noexcept {
        if (off_ >= cap_) {
            return;
        }
        const std::size_t room = cap_ - off_;
        const std::size_t n = s.size() < room ? s.size() : room;
        if (n > 0) {
            std::memcpy(static_cast<void*>(buf_ + off_), static_cast<const void*>(s.data()), n);
            off_ += n;
        }
    }

    /// 追加字符串字面量：长度由编译器推导（含结尾 '\0'，故取 N-1），杜绝手数字节出错。
    /// 注意：不要用它追加 char 缓冲数组（会带上未初始化的尾部），那种场景用 Append(string_view)。
    template <std::size_t N>
    void AppendLit(const char (&lit)[N]) noexcept {
        Append(std::string_view(lit, N - 1));
    }

    void AppendChar(char c) noexcept {
        if (off_ < cap_) {
            buf_[off_++] = c;
        }
    }

    /// 十进制无符号整数，右对齐补 '0' 到 width。
    void AppendUInt(std::uint64_t v, int width) noexcept {
        char tmp[20];
        int len = 0;
        do {
            tmp[len++] = static_cast<char>('0' + static_cast<char>(v % 10u));
            v /= 10u;
        } while (v != 0 && len < 20);
        for (int p = len; p < width; ++p) {
            AppendChar('0');
        }
        while (len > 0) {
            AppendChar(tmp[--len]);
        }
    }

    /// 64 位十六进制，固定 16 字符补零（TraceID / RequestID）。
    void AppendHex16(std::uint64_t v) noexcept {
        for (int shift = 60; shift >= 0; shift -= 4) {
            AppendChar(kHexDigits[static_cast<std::size_t>((v >> shift) & 0xFull)]);
        }
    }

    /// JSON 字符串（带转义）。非 ASCII 按 UTF-8 原样透传。
    void AppendJsonString(std::string_view s) noexcept {
        AppendChar('"');
        for (const char ch : s) {
            switch (ch) {
                case '"':
                    AppendLit(R"(\")");
                    break;
                case '\\':
                    AppendLit(R"(\\)");
                    break;
                case '\n':
                    AppendLit(R"(\n)");
                    break;
                case '\r':
                    AppendLit(R"(\r)");
                    break;
                case '\t':
                    AppendLit(R"(\t)");
                    break;
                default: {
                    const auto c = static_cast<unsigned char>(ch);
                    if (c < 0x20u) {
                        AppendLit(R"(\u00)");
                        AppendChar(kHexDigits[static_cast<std::size_t>((c >> 4) & 0xFu)]);
                        AppendChar(kHexDigits[static_cast<std::size_t>(c & 0xFu)]);
                    } else {
                        AppendChar(ch);
                    }
                    break;
                }
            }
        }
        AppendChar('"');
    }

    std::size_t size() const noexcept { return off_; }

    void Terminate() noexcept {
        if (off_ < cap_) {
            buf_[off_] = '\0';
        } else if (cap_ > 0) {
            buf_[cap_ - 1] = '\0';
        }
    }

private:
    static constexpr char kHexDigits[] = "0123456789abcdef";

    char* buf_;
    std::size_t cap_;
    std::size_t off_{0};
};

/// 把 UTC 纳秒时间戳格式化为 "2026-08-30T07:30:00.123456789Z"（定长 30 字符 + '\0'）。
///
/// 自实现 civil-from-days（Howard Hinnant 算法），不依赖 localtime / tzdb：
/// 无静态缓冲、无锁、可重入、跨平台结果确定，且可由后台线程安全调用。
std::size_t FormatTimestampNs(std::int64_t ns, char* out, std::size_t cap) noexcept;

/// 级别名，补齐到 5 字符，便于文本日志按列对齐。
const char* LevelNamePadded(LogLevel level) noexcept;

/// 文本行（tools/logtrace/parse_trace.py 按 key=value 解析）：
///   <ts> <LEVEL> svc=.. mod=.. trace=.. req=.. player=.. scene=.. tid=.. msg=..
std::size_t FormatTextLine(const LogRecord& rec, char* out, std::size_t cap) noexcept;

/// JSON 行，字段与 LogRecord 一一对应（九项固定字段 + thread_id + 可读 ts）。
std::size_t FormatJsonLine(const LogRecord& rec, char* out, std::size_t cap) noexcept;

}  // namespace mmo::core::detail
