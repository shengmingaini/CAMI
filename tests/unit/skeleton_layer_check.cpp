// tests/unit/skeleton_layer_check.cpp
// 机器可验证证据：五层已接入 monorepo 构建，且 目录↔分层↔target 一一对应。
// 零外部依赖（不依赖 GTest），作为 ctest 用例与 CI 绿灯的直接证据。
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "common/common_layer.h"
#include "data/data_layer.h"
#include "game/game_layer.h"
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

    if (ok) {
        return 0;
    }
    std::fprintf(stderr, "\n[FAIL] 分层接入校验未通过\n");
    return 1;
}
