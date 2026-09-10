// server/dataservice/include/mmo/data/mysql/password_hash.h
//
// TASK-028 · 密码存储（§15.9 / §20.7）。
//
// 硬约束：只存 salted hash。禁止明文、禁止可逆加密、禁止 MD5 等弱哈希。
// 算法：Argon2id（PHC 获胜方案，§15.9 首选），经 pkg-config `libargon2` 引入。
// 编码串自带算法+参数+盐，便于后续**无损升级参数**与新增算法（§27.4 扩展点）。

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "mmo/core/error/result.h"

namespace mmo::data::mysql {

/// Argon2id 参数（默认取 RFC 9106 的第二推荐档，兼顾本地验证成本）。
struct PasswordHashOptions {
    std::uint32_t time_cost{3};       // 迭代次数
    std::uint32_t memory_kib{65536};  // 内存 64MiB
    std::uint32_t parallelism{1};
    std::size_t salt_bytes{16};
};

/// 生成 `$argon2id$v=19$m=...,t=...,p=...$<salt_b64>$<hash_b64>` 编码串。
core::Result<std::string> HashPassword(std::string_view password);
core::Result<std::string> HashPassword(std::string_view password, const PasswordHashOptions& opt);

/// 校验密码。encoded 非法/为空一律返回 false（不区分「格式错」与「密码错」，避免信息泄露）。
bool VerifyPassword(std::string_view password, std::string_view encoded);

/// 编码串是否为 argon2 形态（供 §20.7「grep 无明文密码存储」自证与断言使用）。
bool IsArgon2idEncoded(std::string_view encoded) noexcept;

/// 算法标识（写入 account.password_hash 的前缀用）。
inline constexpr char kPasswordAlgo[] = "argon2id";

}  // namespace mmo::data::mysql
