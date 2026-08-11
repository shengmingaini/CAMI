#include "gateway/security/security_selfcheck.h"
#include "gateway/security/token_auth.h"
#include "gateway/security/aes_gcm.h"

#include <cstdio>
#include <vector>

namespace cami::gateway::security {

// 返回约定与兄弟模块一致：true = 校验通过（selfcheck 成功）。
// 注意：早期实现返回 int 且 0=成功，与骨架 `if (!selfcheck())` 的 bool 约定相反，
// 导致"成功反而判失败"。此处统一为 bool，避免再次踩坑。
bool security_selfcheck() {
    bool ok = true;

    // --- Token 鉴权（始终可用：内存版，OFF/ON 均测）---
    {
        TokenAuth auth(/*ttl_ms=*/1000);
        std::string tok = auth.issue(12345);
        TokenVerifyResult v = auth.verify(tok);
        if (v.status != SecResult::kOk || v.player_id != 12345) {
            std::fprintf(stderr, "[FAIL] security: token issue/verify round-trip\n");
            ok = false;
        } else {
            std::printf("[ OK ] security: token issue/verify round-trip (player=%llu)\n",
                        (unsigned long long)v.player_id);
        }
        // 未知 token 必须拒绝
        if (auth.verify("deadbeef").status != SecResult::kErrTokenUnknown) {
            std::fprintf(stderr, "[FAIL] security: unknown token should be rejected\n");
            ok = false;
        }
        // 吊销路径等价于过期拒绝（过期时间戳逻辑由单测 sleep 场景覆盖）
        auth.revoke(tok);
        if (auth.verify(tok).status != SecResult::kErrTokenUnknown) {
            std::fprintf(stderr, "[FAIL] security: revoked token should be rejected\n");
            ok = false;
        } else {
            std::printf("[ OK ] security: revoked token rejected\n");
        }
    }

    // --- AES-GCM 加解密 ---
#ifdef CAMI_BUILD_MODULES
    {
        std::vector<uint8_t> key(kAesKeyBytes, 0x42);
        std::vector<uint8_t> pt = {'h', 'e', 'l', 'l', 'o'};
        std::vector<uint8_t> nonce, ct;
        if (!AesGcmCipher::encrypt(key, pt, nonce, ct)) {
            std::fprintf(stderr, "[FAIL] security: AES-GCM encrypt\n");
            ok = false;
        } else {
            std::vector<uint8_t> out;
            if (!AesGcmCipher::decrypt(key, nonce, ct, out) || out != pt) {
                std::fprintf(stderr, "[FAIL] security: AES-GCM decrypt round-trip\n");
                ok = false;
            } else {
                // 篡改检测：翻转一个密文字节，decrypt 必须失败
                std::vector<uint8_t> ct2 = ct;
                ct2[0] ^= 0xFF;
                std::vector<uint8_t> out2;
                if (AesGcmCipher::decrypt(key, nonce, ct2, out2)) {
                    std::fprintf(stderr, "[FAIL] security: tampered ciphertext should fail\n");
                    ok = false;
                } else {
                    std::printf("[ OK ] security: AES-GCM round-trip + tamper-detect\n");
                }
            }
        }
    }
#else
    std::printf("[DISABLED] security: AES-GCM not built (CAMI_BUILD_MODULES=OFF); "
                "crypto self-check skipped (real path verified under MODULES=ON)\n");
#endif

    return ok;
}

} // namespace cami::gateway::security
