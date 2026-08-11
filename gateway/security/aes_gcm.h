#pragma once
#include <vector>
#include "gateway/security/security_types.h"

namespace cami::gateway::security {

// AES-256-GCM 对称加密封装。
// 输出布局: ciphertext_out = [ciphertext(=plaintext.len) || tag(16)]；
// decrypt 的输入必须为相同布局，且 nonce 长度须 = kAesNonceBytes。
//
// 真实实现依赖 OpenSSL（CAMI_BUILD_MODULES=ON，vcpkg 已声明 openssl）。
// CAMI_BUILD_MODULES=OFF 下 encrypt/decrypt 直接返回 false（kErrDisabled），
// 由 selfcheck 据此标记 [DISABLED]——符合"轻量 CI 仅 Boost、重型依赖默认关闭"规则。
class AesGcmCipher {
public:
    static bool encrypt(const std::vector<uint8_t>& key,
                        const std::vector<uint8_t>& plaintext,
                        std::vector<uint8_t>& nonce_out,
                        std::vector<uint8_t>& ciphertext_out);

    static bool decrypt(const std::vector<uint8_t>& key,
                        const std::vector<uint8_t>& nonce,
                        const std::vector<uint8_t>& ciphertext_with_tag,
                        std::vector<uint8_t>& plaintext_out);
};

} // namespace cami::gateway::security
