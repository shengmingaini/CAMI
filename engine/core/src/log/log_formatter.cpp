// engine/core/src/log/log_formatter.cpp — TASK-002 日志行格式化（内部实现）
//
// 只被 Logger 的后台刷盘线程调用（单线程），因此无需加锁、无需静态缓冲。
// 全部写入调用方提供的固定缓冲，运行期零分配（§22）。

#include "log/log_formatter.h"

#include "mmo/core/log/log_level.h"

namespace mmo::core::detail {
namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kSecondsPerDay = 86400LL;

struct CivilDate {
    int year;
    std::uint32_t month;  // [1, 12]
    std::uint32_t day;    // [1, 31]
};

/// 天数 -> 年月日（Howard Hinnant 的 civil_from_days，1970-01-01 为第 0 天）。
/// 自实现而非调用 localtime：无静态缓冲、无锁、可重入，结果与操作系统时区设置无关。
CivilDate CivilFromDays(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;                          // [0, 146096]
    const std::int64_t yoe =                                            // [0, 399]
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);   // [0, 365]
    const std::int64_t mp = (5 * doy + 2) / 153;                        // [0, 11]
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;                // [1, 31]
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;                   // [1, 12]
    const int year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    return CivilDate{year, static_cast<std::uint32_t>(m), static_cast<std::uint32_t>(d)};
}

/// 向下取整除法 + 非负余数。
std::int64_t FloorDiv(std::int64_t a, std::int64_t b, std::int64_t& rem) noexcept {
    std::int64_t q = a / b;
    rem = a % b;
    if (rem < 0) {
        rem += b;
        q -= 1;
    }
    return q;
}

const char* SafeCStr(const char* s) noexcept { return s != nullptr ? s : ""; }

}  // namespace

std::size_t FormatTimestampNs(std::int64_t ns, char* out, std::size_t cap) noexcept {
    if (cap < 31) {  // "YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ" = 30 字符 + '\0'
        if (cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    std::int64_t frac = 0;
    const std::int64_t secs = FloorDiv(ns, kNanosPerSecond, frac);
    std::int64_t sod = 0;
    const std::int64_t days = FloorDiv(secs, kSecondsPerDay, sod);
    const CivilDate c = CivilFromDays(days);

    LineWriter w(out, cap);
    w.AppendUInt(c.year > 0 ? static_cast<std::uint64_t>(c.year) : 0u, 4);
    w.AppendChar('-');
    w.AppendUInt(static_cast<std::uint64_t>(c.month), 2);
    w.AppendChar('-');
    w.AppendUInt(static_cast<std::uint64_t>(c.day), 2);
    w.AppendChar('T');
    w.AppendUInt(static_cast<std::uint64_t>(sod / 3600), 2);
    w.AppendChar(':');
    w.AppendUInt(static_cast<std::uint64_t>((sod % 3600) / 60), 2);
    w.AppendChar(':');
    w.AppendUInt(static_cast<std::uint64_t>(sod % 60), 2);
    w.AppendChar('.');
    w.AppendUInt(static_cast<std::uint64_t>(frac), 9);
    w.AppendChar('Z');
    w.Terminate();
    return w.size();
}

const char* LevelNamePadded(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warn:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
    }
    return "UNKNOWN";
}

std::size_t FormatTextLine(const LogRecord& rec, char* out, std::size_t cap) noexcept {
    char ts[32];
    FormatTimestampNs(rec.timestamp_ns, ts, sizeof(ts));

    LineWriter w(out, cap);
    w.Append(std::string_view(ts));
    w.AppendChar(' ');
    w.Append(LevelNamePadded(rec.level));
    w.AppendLit(" svc=");
    w.Append(SafeCStr(rec.service));
    w.AppendLit(" mod=");
    w.Append(SafeCStr(rec.module));
    w.AppendLit(" trace=");
    w.AppendHex16(rec.trace_id);
    w.AppendLit(" req=");
    w.AppendHex16(rec.request_id);
    w.AppendLit(" player=");
    if (rec.player_id == kInvalidPlayerId) {
        w.AppendChar('-');
    } else {
        w.AppendUInt(rec.player_id, 1);
    }
    w.AppendLit(" scene=");
    if (rec.scene_id == kInvalidSceneId) {
        w.AppendChar('-');
    } else {
        w.AppendUInt(rec.scene_id, 1);
    }
    w.AppendLit(" tid=");
    w.AppendUInt(static_cast<std::uint64_t>(rec.thread_id), 1);
    w.AppendLit(" msg=");
    w.Append(std::string_view(rec.message, rec.message_len));
    w.Terminate();
    return w.size();
}

std::size_t FormatJsonLine(const LogRecord& rec, char* out, std::size_t cap) noexcept {
    char ts[32];
    FormatTimestampNs(rec.timestamp_ns, ts, sizeof(ts));

    LineWriter w(out, cap);
    w.AppendLit(R"({"ts_ns":)");
    w.AppendUInt(static_cast<std::uint64_t>(rec.timestamp_ns), 1);
    w.AppendLit(R"(,"ts":")");
    w.Append(std::string_view(ts));
    w.AppendLit(R"(","level":")");
    w.Append(ToString(rec.level));
    w.AppendLit(R"(","service":)");
    w.AppendJsonString(SafeCStr(rec.service));
    w.AppendLit(R"(,"module":)");
    w.AppendJsonString(SafeCStr(rec.module));
    w.AppendLit(R"(,"trace_id":")");
    w.AppendHex16(rec.trace_id);
    w.AppendLit(R"(","request_id":")");
    w.AppendHex16(rec.request_id);
    w.AppendLit(R"(","player_id":)");
    if (rec.player_id == kInvalidPlayerId) {
        w.AppendLit("null");
    } else {
        w.AppendUInt(rec.player_id, 1);
    }
    w.AppendLit(R"(,"scene_id":)");
    if (rec.scene_id == kInvalidSceneId) {
        w.AppendLit("null");
    } else {
        w.AppendUInt(rec.scene_id, 1);
    }
    w.AppendLit(R"(,"thread_id":)");
    w.AppendUInt(static_cast<std::uint64_t>(rec.thread_id), 1);
    w.AppendLit(R"(,"message":)");
    w.AppendJsonString(std::string_view(rec.message, rec.message_len));
    w.AppendChar('}');
    w.Terminate();
    return w.size();
}

}  // namespace mmo::core::detail
