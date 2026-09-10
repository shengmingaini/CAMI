// server/dataservice/src/mysql/password_hash.cpp
//
// TASK-028 §15.9 / §20.7 · 密码 salted hash 实现（argon2id）。
//
// 只存不可逆编码串 `$argon2id$v=19$m=<kib>,t=<iter>,p=<par>$<salt_b64>$<hash_b64>`：
// 盐随密码随机生成（OpenSSL CSPRNG），严禁明文、严禁可逆加密、严禁 MD5 等弱哈希。

#include "mmo/data/mysql/password_hash.h"

#include <argon2.h>
#include <openssl/rand.h>

#include <string>
#include <vector>

namespace mmo::data::mysql {

namespace {

constexpr std::uint32_t kHashBytes = 32;  // 256 bit

core::Error Err(core::ErrorCode code, const char* msg) {
    return core::Error(code, msg, core::domain::kData);
}

std::vector<std::uint8_t> RandomSalt(std::size_t n) {
    std::vector<std::uint8_t> salt(n);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        salt.clear();  // 调用方据此报错，绝不退化为固定/可预测盐
    }
    return salt;
}

}  // namespace

bool IsArgon2idEncoded(std::string_view encoded) noexcept {
    constexpr std::string_view kPrefix = "$argon2id$";
    return encoded.size() > kPrefix.size() &&
           encoded.compare(0, kPrefix.size(), kPrefix) == 0;
}

core::Result<std::string> HashPassword(std::string_view password) {
    return HashPassword(password, PasswordHashOptions{});
}

core::Result<std::string> HashPassword(std::string_view password, const PasswordHashOptions& opt) {
    if (password.empty()) {
        return core::Result<std::string>::Fail(Err(core::ErrorCode::INVALID_ARGUMENT,
                                                   "password must not be empty"));
    }
    if (opt.salt_bytes < 8) {
        return core::Result<std::string>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "salt must be >= 8 bytes"));
    }

    const std::vector<std::uint8_t> salt = RandomSalt(opt.salt_bytes);
    if (salt.empty()) {
        return core::Result<std::string>::Fail(
            Err(core::ErrorCode::INTERNAL_ERROR, "csprng unavailable"));
    }

    const std::size_t encoded_len = argon2_encodedlen(
        opt.time_cost, opt.memory_kib, opt.parallelism,
        static_cast<std::uint32_t>(salt.size()), kHashBytes, Argon2_id);
    std::string encoded(encoded_len, '\0');

    const int rc = argon2id_hash_encoded(
        opt.time_cost, opt.memory_kib, opt.parallelism, password.data(), password.size(),
        salt.data(), salt.size(), kHashBytes, encoded.data(), encoded.size());
    if (rc != ARGON2_OK) {
        return core::Result<std::string>::Fail(
            Err(core::ErrorCode::INTERNAL_ERROR, argon2_error_message(rc)));
    }
    return core::Result<std::string>::Ok(std::move(encoded));
}

bool VerifyPassword(std::string_view password, std::string_view encoded) {
    if (password.empty() || encoded.empty()) return false;
    // argon2id_verify 要求 encoded 为 C 串（string_view 不保证 NUL 结尾），故显式拷贝。
    const std::string enc(encoded);
    // 非法编码串一律 false：不区分「格式错」与「密码错」，避免探测账号是否存在。
    return argon2id_verify(enc.c_str(), password.data(), password.size()) == ARGON2_OK;
}

}  // namespace mmo::data::mysql
