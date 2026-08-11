#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include "gateway/security/security_types.h"

namespace cami::gateway::security {

// 登录 token 鉴权（网关侧）。
//
// Day1 交付：不透明 token + 内存存储，可端到端签发/校验/吊销。
// 周四(Redis)将把内部 TokenStore 替换为 Redis 实现（在线态 / 分布式吊销），
// 对外接口（issue / verify / revoke）保持不变——此处即为预留的集成缝。
class TokenAuth {
public:
    explicit TokenAuth(int64_t ttl_ms = 3600 * 1000);

    // 签发：返回不透明 token 字符串（原型用 32 字节随机值 hex 编码）。
    std::string issue(uint64_t player_id);

    // 校验：成功时 status=kOk 并填充 player_id；失败给出对应 SecResult。
    TokenVerifyResult verify(const std::string& token);

    // 吊销（预留；周四接 Redis 时实现分布式吊销）。
    void revoke(const std::string& token);

private:
    int64_t now_ms() const;
    std::vector<uint8_t> random_bytes(std::size_t n) const;

    int64_t ttl_ms_;
    struct Entry { uint64_t player_id; int64_t expiry_ms; };
    std::unordered_map<std::string, Entry> store_;  // 周四替换为 Redis
};

} // namespace cami::gateway::security
