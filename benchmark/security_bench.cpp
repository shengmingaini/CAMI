// 网关安全校验 · AES-256-GCM 性能基准（Week3 周一）
// 验收: 加解密耗时 < 0.05ms/包。
// 仅 CAMI_BUILD_MODULES=ON 编译（依赖 OpenSSL 硬件 AES-NI）。
#include <chrono>
#include <cstdio>
#include <vector>

#include "gateway/security/aes_gcm.h"
#include "gateway/security/security_types.h"

int main() {
    using namespace cami::gateway::security;
    constexpr int kPackets = 200000;
    constexpr std::size_t kPayload = 256;  // 典型 MMO 业务包体
    constexpr double kLimitUs = 50.0;      // 0.05 ms

    std::vector<uint8_t> key(kAesKeyBytes, 0x5A);
    std::vector<uint8_t> pt(kPayload, 0x7E);
    std::vector<uint8_t> nonce, ct, out;

    // 预热（避免首次 EVP 上下文分配抖动）
    if (!AesGcmCipher::encrypt(key, pt, nonce, ct)) {
        std::fprintf(stderr, "AES-GCM encrypt warmup failed\n");
        return 1;
    }

    double total_us = 0.0;
    for (int i = 0; i < kPackets; ++i) {
        auto a = std::chrono::steady_clock::now();
        AesGcmCipher::encrypt(key, pt, nonce, ct);
        AesGcmCipher::decrypt(key, nonce, ct, out);
        auto b = std::chrono::steady_clock::now();
        total_us += std::chrono::duration<double, std::micro>(b - a).count();
    }

    double per_pkt_us = total_us / kPackets;
    std::printf("AES-256-GCM encrypt+decrypt: %.3f us/packet (%.2f M pkt/s)\n",
                per_pkt_us, 1000.0 / per_pkt_us);

    if (per_pkt_us <= kLimitUs) {
        std::printf("[ PASS ] 加解密耗时 < 0.05ms/包 (验收达成)\n");
        return 0;
    }
    std::printf("[ FAIL ] 加解密耗时 %.3f us/包 超过 50us 验收线\n", per_pkt_us);
    return 1;
}
