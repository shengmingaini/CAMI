#include "gateway/security/aes_gcm.h"

#ifdef CAMI_BUILD_MODULES
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace cami::gateway::security {

bool AesGcmCipher::encrypt(const std::vector<uint8_t>& key,
                           const std::vector<uint8_t>& plaintext,
                           std::vector<uint8_t>& nonce_out,
                           std::vector<uint8_t>& ciphertext_out) {
    if (key.size() != kAesKeyBytes) return false;
    nonce_out.assign(kAesNonceBytes, 0);
    if (RAND_bytes(nonce_out.data(), static_cast<int>(kAesNonceBytes)) != 1) return false;

    ciphertext_out.resize(plaintext.size() + kAesTagBytes);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    int len = 0;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(kAesNonceBytes), nullptr) != 1) break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce_out.data()) != 1) break;
        if (!plaintext.empty()) {
            if (EVP_EncryptUpdate(ctx, ciphertext_out.data(), &len,
                                  plaintext.data(), static_cast<int>(plaintext.size())) != 1) break;
        }
        int ct_len = len;
        if (EVP_EncryptFinal_ex(ctx, ciphertext_out.data() + ct_len, &len) != 1) break;
        ct_len += len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kAesTagBytes),
                                ciphertext_out.data() + plaintext.size()) != 1) break;
        ciphertext_out.resize(plaintext.size() + kAesTagBytes);
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool AesGcmCipher::decrypt(const std::vector<uint8_t>& key,
                           const std::vector<uint8_t>& nonce,
                           const std::vector<uint8_t>& ciphertext_with_tag,
                           std::vector<uint8_t>& plaintext_out) {
    if (key.size() != kAesKeyBytes) return false;
    if (nonce.size() != kAesNonceBytes) return false;
    if (ciphertext_with_tag.size() < kAesTagBytes) return false;

    const std::size_t ct_len = ciphertext_with_tag.size() - kAesTagBytes;
    plaintext_out.resize(ct_len);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    int len = 0;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(kAesNonceBytes), nullptr) != 1) break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) break;
        if (ct_len > 0) {
            if (EVP_DecryptUpdate(ctx, plaintext_out.data(), &len,
                                  ciphertext_with_tag.data(),
                                  static_cast<int>(ct_len)) != 1) break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kAesTagBytes),
                                const_cast<unsigned char*>(
                                    ciphertext_with_tag.data() + ct_len)) != 1) break;
        int pt_len = len;
        if (EVP_DecryptFinal_ex(ctx, plaintext_out.data() + pt_len, &len) != 1) break;
        plaintext_out.resize(static_cast<std::size_t>(pt_len) + static_cast<std::size_t>(len));
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

} // namespace cami::gateway::security

#else // !CAMI_BUILD_MODULES —— 占位实现：加密不可用，selfcheck 据此标记 DISABLED
namespace cami::gateway::security {
bool AesGcmCipher::encrypt(const std::vector<uint8_t>&,
                           const std::vector<uint8_t>&,
                           std::vector<uint8_t>&,
                           std::vector<uint8_t>&) { return false; }
bool AesGcmCipher::decrypt(const std::vector<uint8_t>&,
                           const std::vector<uint8_t>&,
                           const std::vector<uint8_t>&,
                           std::vector<uint8_t>&) { return false; }
} // namespace cami::gateway::security
#endif
