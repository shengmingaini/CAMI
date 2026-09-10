// server/dataservice/src/redis/serialization.cpp
//
// TASK-027 · Record <-> Redis 值 二进制序列化（见 serialization.h 格式说明）。

#include "mmo/data/redis/serialization.h"

#include <cstring>

namespace mmo::data::redis {

namespace {

constexpr char kMagic[4] = {'M', 'M', 'O', '1'};
constexpr std::size_t kHeader = 4 + 4 + 8 + 4 + 4;  // magic/version/ns/keylen/paylen

inline void PutLE(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
inline void PutLE(std::string& out, std::int64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
inline std::uint32_t GetLE32(std::string_view b, std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + i])) << (8 * i);
    return v;
}
inline std::int64_t GetLE64(std::string_view b, std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(b[off + i])) << (8 * i);
    return static_cast<std::int64_t>(v);
}

}  // namespace

std::string EncodeRecord(const Record& rec) {
    std::string out;
    out.reserve(kHeader + rec.key.size() + rec.payload.size());
    out.append(kMagic, 4);
    PutLE(out, rec.version);
    PutLE(out, static_cast<std::int64_t>(rec.updated_at.time_since_epoch().count()));
    PutLE(out, static_cast<std::uint32_t>(rec.key.size()));
    out.append(rec.key);
    PutLE(out, static_cast<std::uint32_t>(rec.payload.size()));
    out.append(rec.payload);
    return out;
}

core::Result<Record> DecodeRecord(std::string_view blob) {
    if (blob.size() < kHeader) {
        return core::Result<Record>::Fail(core::Error(core::ErrorCode::INTERNAL_ERROR,
            "redis value too short", core::domain::kData));
    }
    if (std::memcmp(blob.data(), kMagic, 4) != 0) {
        return core::Result<Record>::Fail(core::Error(core::ErrorCode::INTERNAL_ERROR,
            "redis value bad magic", core::domain::kData));
    }
    const std::uint32_t key_len = GetLE32(blob, 16);
    // 读取 pay_len 字段前先确认长度足够，避免越界（§16 损坏容错）
    if (blob.size() < 24 + key_len) {
        return core::Result<Record>::Fail(core::Error(core::ErrorCode::INTERNAL_ERROR,
            "redis value truncated", core::domain::kData));
    }
    const std::uint32_t pay_len = GetLE32(blob, 20 + key_len);
    if (key_len > (1u << 20) || pay_len > (1u << 28)) {
        return core::Result<Record>::Fail(core::Error(core::ErrorCode::INTERNAL_ERROR,
            "redis value length overflow", core::domain::kData));
    }
    if (blob.size() != kHeader + key_len + pay_len) {
        return core::Result<Record>::Fail(core::Error(core::ErrorCode::INTERNAL_ERROR,
            "redis value length mismatch", core::domain::kData));
    }
    Record r;
    r.version = GetLE32(blob, 4);
    r.updated_at = mmo::core::SteadyTime(
        std::chrono::nanoseconds(GetLE64(blob, 8)));
    r.key.assign(blob.substr(20, key_len));
    r.payload.assign(blob.substr(24 + key_len, pay_len));
    return core::Result<Record>::Ok(std::move(r));
}

}  // namespace mmo::data::redis
