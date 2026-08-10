#pragma once
#include <cstddef>
#include <initializer_list>

namespace cami {
namespace common {

// 全局公共服务层 (spec §3.1 / §4 隐含)
inline constexpr char kLayerName[] = "common";
// 本层不依赖其他业务层（最底层公共服务）
inline constexpr std::initializer_list<const char*> kDependsOn = {};
inline constexpr std::size_t kDependsOnSize = 0;

// 链接锚点声明（定义见 .cpp），skeleton 自检调用以强制链接本层符号。
const char* layer_name();
const char* depends_on();

// 事件总线功能自检（Day5 交付物）：发布→drain→分发到订阅者，返回是否一致。
// 被 tests/unit/skeleton_layer_check.cpp 调用，作为事件总线已接入并可用的机器证据。
bool event_bus_selfcheck();

}  // namespace common
}  // namespace cami
