#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "mmo/core/error/result.h"

namespace mmo::core {

/// Uuid —— 128 位通用唯一标识符（RFC 9562 V4 / V7）。
///
/// State Owner  : 值类型，无共享状态；生成器状态按线程局部持有（见 uuid.cpp）。
/// Hot Path     : YES —— MessageID / TransactionID 在消息创建路径上生成。
/// Thread Safety: 全部成员 const 或作用于自身；NewV4/NewV7 无全局锁、无共享随机引擎。
///
/// 排序语义：operator<=> 为字节字典序。对 V7 而言等价于「时间有序」，
/// 可直接用于数据库主键；对 V4 而言无业务含义，但比较仍然良定义。
///
/// 熵源失败约定：TryNewV4 / TryNewV7 返回 ErrorCode::INTERNAL_ERROR；
/// NewV4 / NewV7 为任务书 §7 指定的便捷入口（无法返回错误），
/// 熵源失败时返回 Nil（全零），**绝不降级为弱随机 UUID**。
class Uuid {
public:
    /// 16 字节原始表示（任务书 §7 指定为公开成员）。
    std::array<std::uint8_t, 16> bytes{};

    /// 全零 UUID，用作「无效值」哨兵。
    static Uuid Nil() noexcept { return Uuid{}; }

    /// V4（随机）：用于 MessageID / TransactionID / RequestID 等无顺序要求的标识。
    static Result<Uuid> TryNewV4() noexcept;
    static Uuid NewV4() noexcept;

    /// V7（时间有序）：用于数据库主键、需要按创建时间排序的场景。
    static Result<Uuid> TryNewV7() noexcept;
    static Uuid NewV7() noexcept;

    /// 规范文本形式："xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"（小写十六进制）。
    [[nodiscard]] std::string ToString() const;

    /// 解析：接受 36 字符带连字符形式，也接受 32 字符纯十六进制；大小写不敏感。
    /// 格式非法返回 ErrorCode::INVALID_ARGUMENT。
    static Result<Uuid> Parse(std::string_view text);

    /// RFC version 字段（bytes[6] 高 4 位）：V4 = 4，V7 = 7。
    [[nodiscard]] std::uint8_t Version() const noexcept { return static_cast<std::uint8_t>(bytes[6] >> 4); }

    /// RFC variant 字段（bytes[8] 高 2 位）：RFC 9562 恒为 0b10 = 2。
    [[nodiscard]] std::uint8_t Variant() const noexcept { return static_cast<std::uint8_t>(bytes[8] >> 6); }

    [[nodiscard]] bool IsNil() const noexcept { return *this == Uuid{}; }

    /// V7 内嵌的 Unix 毫秒时间戳；V4 返回无意义值（不报错，调用方自行判别 Version）。
    [[nodiscard]] std::int64_t TimestampMillis() const noexcept;

    bool operator==(const Uuid&) const noexcept = default;
    auto operator<=>(const Uuid&) const noexcept = default;
};

}  // namespace mmo::core
