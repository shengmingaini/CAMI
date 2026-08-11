// tests/unit/skeleton_layer_check.cpp
// 机器可验证证据：五层已接入 monorepo 构建，且 目录↔分层↔target 一一对应。
// 零外部依赖（不依赖 GTest），作为 ctest 用例与 CI 绿灯的直接证据。
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "common/common_layer.h"
#include "data/data_layer.h"
#include "game/game_layer.h"
#include "gateway/codec/codec_selfcheck.h"
#include "gateway/connection/connection_selfcheck.h"
#include "gateway/heartbeat/heartbeat_selfcheck.h"
#include "gateway/security/security_selfcheck.h"
#include "gateway/ratelimit/ratelimit_selfcheck.h"
#include "gateway/router/router_selfcheck.h"
#include "gateway/redis/redis_selfcheck.h"
#include "gateway/integration/gateway_pipeline_selfcheck.h"
#include "gateway/gateway_layer.h"
#include "ops/ops_layer.h"

int main() {
    // 强制引用每层符号，确保各层 target 真实链接进二进制。
    // 任一层未接入构建 → CMake 配置期 target 缺失 或 链接期符号缺失 → CI 红。
    const char* p0 = cami::common::layer_name();
    const char* p1 = cami::data::layer_name();
    const char* p2 = cami::game::layer_name();
    const char* p3 = cami::gateway::layer_name();
    const char* p4 = cami::ops::layer_name();
    (void)p0; (void)p1; (void)p2; (void)p3; (void)p4;

    struct LayerInfo {
        const char* name;
        std::size_t deps;
    };
    const LayerInfo layers[] = {
        {cami::common::kLayerName,  cami::common::kDependsOnSize},
        {cami::data::kLayerName,    cami::data::kDependsOnSize},
        {cami::game::kLayerName,    cami::game::kDependsOnSize},
        {cami::gateway::kLayerName, cami::gateway::kDependsOnSize},
        {cami::ops::kLayerName,     cami::ops::kDependsOnSize},
    };
    const char* expected[] = {"common", "data", "game", "gateway", "ops"};

    bool ok = true;
    for (int i = 0; i < 5; ++i) {
        if (std::strcmp(layers[i].name, expected[i]) != 0) {
            std::fprintf(stderr, "[FAIL] layer[%d] = '%s', expected '%s'\n",
                         i, layers[i].name, expected[i]);
            ok = false;
        } else {
            std::printf("[ OK ] %-8s linked into build (deps=%zu)\n",
                        layers[i].name, layers[i].deps);
        }
    }

    if (ok) {
        std::printf(
            "\n=== CAMI 五层架构已接入 monorepo 构建：目录↔分层↔target 一一对应 ===\n");
    }

    // Day5 交付物验证：事件总线已接入且功能正常（发布→drain→分发）。
    if (!cami::common::event_bus_selfcheck()) {
        std::fprintf(stderr, "[FAIL] event_bus_selfcheck(): 发布/分发链路异常\n");
        ok = false;
    } else {
        std::printf("[ OK ] event_bus functional (publish -> drain -> dispatch)\n");
    }

    // Week2 周一交付物验证：连接管理模块 FSM 迁移合法性 + io_context 线程池启停。
    if (!cami::gateway::connection::connection_selfcheck()) {
        std::fprintf(stderr, "[FAIL] connection_selfcheck(): FSM/线程池异常\n");
        ok = false;
    } else {
        std::printf("[ OK ] connection_manager functional (fsm + iocontext pool)\n");
    }

    // Week2 周二交付物验证：协议编解码帧定界（粘包/半包/超大包/边界）。
    if (!cami::gateway::codec::codec_selfcheck()) {
        std::fprintf(stderr, "[FAIL] codec_selfcheck(): 帧定界逻辑异常\n");
        ok = false;
    } else {
        std::printf("[ OK ] codec functional (sticky/half/oversized framing)\n");
    }

    // Week2 周三交付物验证：心跳管理（保活/超时踢线/无泄漏/空闲回收）。
    if (!cami::gateway::heartbeat::heartbeat_selfcheck()) {
        std::fprintf(stderr, "[FAIL] heartbeat_selfcheck(): 心跳逻辑异常\n");
        ok = false;
    } else {
        std::printf("[ OK ] heartbeat manager functional (keepalive / timeout-kick / idle-recycle)\n");
    }

    // Week3 周一交付物验证：安全校验（token 鉴权；AES-GCM 在 MODULES=ON 实跑，OFF 标记 DISABLED）。
    if (!cami::gateway::security::security_selfcheck()) {
        std::fprintf(stderr, "[FAIL] security_selfcheck(): token/加密异常\n");
        ok = false;
    } else {
        std::printf("[ OK ] security functional (token auth; AES-GCM selfcheck)\n");
    }

    // Week3 周二交付物验证：限流防攻击（令牌桶 + 连接频率 + 黑白名单，确定性 selfcheck）。
    if (!cami::gateway::ratelimit::ratelimit_selfcheck()) {
        std::fprintf(stderr, "[FAIL] ratelimit_selfcheck(): 限流逻辑异常\n");
        ok = false;
    } else {
        std::printf("[ OK ] ratelimit functional (token-bucket + conn-freq + ip-list)\n");
    }

    // Week3 周三交付物验证：路由（一致性哈希 + 热更新；迁移 <10%、负载均衡、确定性 selfcheck）。
    if (!cami::gateway::router::router_selfcheck()) {
        std::fprintf(stderr, "[FAIL] router_selfcheck(): 路由逻辑异常\n");
        ok = false;
    } else {
        std::printf("[ OK ] router functional (consistent-hash + hot-reload)\n");
    }

    // Week3 周四交付物验证：在线态存储（内存后端语义 + 16 分片均衡 + 故障切换重路由，确定性 selfcheck）。
    if (!cami::gateway::redis::redis_selfcheck()) {
        std::fprintf(stderr, "[FAIL] redis_selfcheck(): 在线态/分片逻辑异常\n");
        ok = false;
    } else {
        std::printf("[ OK ] redis functional (in-memory backend + 16-shard + failover)\n");
    }

    // Week3 集成层验证：四模块集成缝组合成可单测策略（限流/鉴权/选后端/在线态）。
    if (!cami::gateway::integration::gateway_pipeline_selfcheck()) {
        std::fprintf(stderr, "[FAIL] gateway_pipeline_selfcheck(): 集成缝逻辑异常\n");
        ok = false;
    } else {
        std::printf("[ OK ] integration pipeline functional (ratelimit+security+router+redis seams)\n");
    }

    if (ok) {
        return 0;
    }
    std::fprintf(stderr, "\n[FAIL] 分层接入校验未通过\n");
    return 1;
}
