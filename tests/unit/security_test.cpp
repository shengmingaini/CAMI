#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "gateway/security/token_auth.h"
#include "gateway/security/aes_gcm.h"

using namespace cami::gateway::security;

TEST(SecurityToken, IssueVerifyRoundTrip) {
    TokenAuth auth(/*ttl_ms=*/60 * 1000);
    std::string tok = auth.issue(99887766);
    TokenVerifyResult v = auth.verify(tok);
    EXPECT_EQ(v.status, SecResult::kOk);
    EXPECT_EQ(v.player_id, 99887766u);
}

TEST(SecurityToken, UnknownRejected) {
    TokenAuth auth(60 * 1000);
    EXPECT_EQ(auth.verify("nope").status, SecResult::kErrTokenUnknown);
}

TEST(SecurityToken, RevokedRejected) {
    TokenAuth auth(60 * 1000);
    std::string tok = auth.issue(1);
    auth.revoke(tok);
    EXPECT_EQ(auth.verify(tok).status, SecResult::kErrTokenUnknown);
}

TEST(SecurityToken, ExpiredRejected) {
    TokenAuth auth(/*ttl_ms=*/1);  // 1ms TTL
    std::string tok = auth.issue(7);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(auth.verify(tok).status, SecResult::kErrTokenExpired);
}

#ifdef CAMI_BUILD_MODULES
TEST(SecurityAesGcm, RoundTrip) {
    std::vector<uint8_t> key(kAesKeyBytes, 0x11);
    std::vector<uint8_t> pt(256, 0xAB);
    std::vector<uint8_t> nonce, ct;
    ASSERT_TRUE(AesGcmCipher::encrypt(key, pt, nonce, ct));
    std::vector<uint8_t> out;
    ASSERT_TRUE(AesGcmCipher::decrypt(key, nonce, ct, out));
    EXPECT_EQ(out, pt);

    // 篡改检测
    std::vector<uint8_t> ct2 = ct;
    ct2[0] ^= 0xFF;
    std::vector<uint8_t> out2;
    EXPECT_FALSE(AesGcmCipher::decrypt(key, nonce, ct2, out2));
}
#else
TEST(SecurityAesGcm, DisabledInOffBuild) {
    std::vector<uint8_t> key(kAesKeyBytes, 0x11);
    std::vector<uint8_t> pt = {1, 2, 3};
    std::vector<uint8_t> nonce, ct;
    EXPECT_FALSE(AesGcmCipher::encrypt(key, pt, nonce, ct));
    EXPECT_FALSE(AesGcmCipher::decrypt(key, nonce, ct, ct));
}
#endif
