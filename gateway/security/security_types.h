#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cami::gateway::security {

// === AES-256-GCM 参数（GCM 标准）===
constexpr std::size_t kAesKeyBytes = 32;    // AES-256
constexpr std::size_t kAesNonceBytes = 12;  // GCM 推荐 IV 长度
constexpr std::size_t kAesTagBytes = 16;    // GCM 认证标签
constexpr std::size_t kMaxPacketBytes = 65536;

enum class SecResult : int {
    kOk = 0,
    kErrDisabled = 1,     // 加密在 CAMI_BUILD_MODULES=OFF 下不可用
    kErrKeySize = 2,
    kErrEncrypt = 3,
    kErrDecrypt = 4,      // 含标签校验失败 / 密文被篡改
    kErrBadToken = 5,
    kErrTokenExpired = 6,
    kErrTokenUnknown = 7,
};

// 登录 token 鉴权结果
struct TokenVerifyResult {
    SecResult status = SecResult::kErrTokenUnknown;
    uint64_t player_id = 0;
};

// 安全配置（原型：TTL 与密钥来源；生产密钥应来自 KMS / 配置中心，禁止硬编码）
struct SecurityConfig {
    std::vector<uint8_t> master_key;       // AES-256 密钥，长度须 = kAesKeyBytes
    int64_t token_ttl_ms = 3600 * 1000;    // token 默认有效期 1h
};

} // namespace cami::gateway::security
