#include "gateway/security/token_auth.h"

#include <chrono>
#include <random>

#ifdef CAMI_BUILD_MODULES
#include <openssl/rand.h>
#endif

namespace cami::gateway::security {

namespace {
    std::string to_hex(const std::vector<uint8_t>& b) {
        static const char* d = "0123456789abcdef";
        std::string s;
        s.reserve(b.size() * 2);
        for (unsigned char c : b) {
            s.push_back(d[c >> 4]);
            s.push_back(d[c & 0x0f]);
        }
        return s;
    }
}

TokenAuth::TokenAuth(int64_t ttl_ms) : ttl_ms_(ttl_ms) {}

int64_t TokenAuth::now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::vector<uint8_t> TokenAuth::random_bytes(std::size_t n) const {
    std::vector<uint8_t> out(n, 0);
#ifdef CAMI_BUILD_MODULES
    RAND_bytes(out.data(), static_cast<int>(n));
#else
    // 原型用 std::random_device（非 CSPRNG 强保障，仅本地原型；生产应走 OpenSSL RAND_bytes）。
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (auto& b : out) b = static_cast<uint8_t>(gen() & 0xff);
#endif
    return out;
}

std::string TokenAuth::issue(uint64_t player_id) {
    std::string token = to_hex(random_bytes(32));
    store_[token] = Entry{player_id, now_ms() + ttl_ms_};
    return token;
}

TokenVerifyResult TokenAuth::verify(const std::string& token) {
    auto it = store_.find(token);
    if (it == store_.end()) return {SecResult::kErrTokenUnknown, 0};
    if (it->second.expiry_ms <= now_ms()) {
        store_.erase(it);  // 惰性过期清理
        return {SecResult::kErrTokenExpired, 0};
    }
    return {SecResult::kOk, it->second.player_id};
}

void TokenAuth::revoke(const std::string& token) {
    store_.erase(token);
}

} // namespace cami::gateway::security
