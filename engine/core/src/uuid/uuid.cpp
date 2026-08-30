#include "mmo/core/uuid/uuid.h"

#include <cstddef>
#include <cstdint>

#include "mmo/core/error/error.h"
#include "mmo/core/time/clock.h"
#include "uuid/entropy.h"

namespace mmo::core {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// 单字符十六进制 -> 数值；非法返回 -1。
int HexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return (c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (c - 'A') + 10;
    }
    return -1;
}

void WriteHex(std::uint8_t value, char* out) noexcept {
    out[0] = kHexDigits[(value >> 4) & 0x0F];
    out[1] = kHexDigits[value & 0x0F];
}

}  // namespace

Result<Uuid> Uuid::TryNewV4() noexcept {
    Uuid out{};
    const Result<void> fill = uuid_internal::FillRandom(out.bytes.data(), out.bytes.size());
    if (!fill.HasValue()) {
        return Result<Uuid>::Fail(fill.Err());
    }
    // version = 0100 (V4)，variant = 10 (RFC 9562)
    out.bytes[6] = static_cast<std::uint8_t>((out.bytes[6] & 0x0FU) | 0x40U);
    out.bytes[8] = static_cast<std::uint8_t>((out.bytes[8] & 0x3FU) | 0x80U);
    return Result<Uuid>::Ok(out);
}

Uuid Uuid::NewV4() noexcept {
    const Result<Uuid> result = TryNewV4();
    // 熵源失败返回 Nil（全零）而不是弱随机 UUID：宁可显式无效，不可静默降级。
    return result.HasValue() ? result.Value() : Uuid::Nil();
}

Result<Uuid> Uuid::TryNewV7() noexcept {
    Uuid out{};
    // 只给随机部分（bytes[6..15]，10 字节）取熵：前 6 字节会被时间戳整体覆盖，
    // 没必要为注定被丢弃的字节付 CSPRNG 开销。随机位仍是 80 - 6 = 74 位。
    constexpr std::size_t kRandomOffset = 6;
    const Result<void> fill = uuid_internal::FillRandom(out.bytes.data() + kRandomOffset,
                                                       out.bytes.size() - kRandomOffset);
    if (!fill.HasValue()) {
        return Result<Uuid>::Fail(fill.Err());
    }
    // 前 48 位 = 大端 Unix 毫秒，保证按创建时间有序（数据库主键友好）
    const auto millis = static_cast<std::uint64_t>(WallClock::UnixMillis());
    for (int i = 0; i < 6; ++i) {
        const auto shift = static_cast<unsigned>(40 - (8 * i));
        out.bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((millis >> shift) & 0xFFU);
    }
    // version = 0111 (V7)，variant = 10 (RFC 9562)
    out.bytes[6] = static_cast<std::uint8_t>((out.bytes[6] & 0x0FU) | 0x70U);
    out.bytes[8] = static_cast<std::uint8_t>((out.bytes[8] & 0x3FU) | 0x80U);
    return Result<Uuid>::Ok(out);
}

Uuid Uuid::NewV7() noexcept {
    const Result<Uuid> result = TryNewV7();
    return result.HasValue() ? result.Value() : Uuid::Nil();
}

std::string Uuid::ToString() const {
    std::string out(36, '\0');
    std::size_t pos = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        // 8-4-4-4-12 分组：在第 4/6/8/10 个字节前插连字符
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[pos] = '-';
            ++pos;
        }
        WriteHex(bytes[i], &out[pos]);
        pos += 2;
    }
    return out;
}

Result<Uuid> Uuid::Parse(std::string_view text) {
    auto fail = []() -> Result<Uuid> {
        return Result<Uuid>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "uuid: bad format"));
    };

    const bool dashed = (text.size() == 36);
    if (!dashed && text.size() != 32) {
        return fail();
    }
    if (dashed && (text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')) {
        return fail();
    }

    Uuid out{};
    std::size_t byte_index = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '-') {
            ++i;
            continue;
        }
        if ((byte_index >= out.bytes.size()) || ((i + 1) >= text.size())) {
            return fail();
        }
        const int hi = HexValue(text[i]);
        const int lo = HexValue(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return fail();
        }
        out.bytes[byte_index] = static_cast<std::uint8_t>((hi << 4) | lo);
        ++byte_index;
        i += 2;
    }
    if (byte_index != out.bytes.size()) {
        return fail();
    }
    return Result<Uuid>::Ok(out);
}

std::int64_t Uuid::TimestampMillis() const noexcept {
    std::uint64_t millis = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        millis = (millis << 8) | static_cast<std::uint64_t>(bytes[i]);
    }
    return static_cast<std::int64_t>(millis);
}

}  // namespace mmo::core
