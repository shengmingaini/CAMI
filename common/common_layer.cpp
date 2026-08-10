#include "common/common_layer.h"
#include "common/event_bus/event_bus.h"

#include <atomic>

namespace cami {
namespace common {

// 链接锚点：确保本层符号被编译进静态库，构建期可验证层真实存在。
const char* layer_name() { return kLayerName; }
const char* depends_on() { return kDependsOnSize ? *kDependsOn.begin() : "none"; }

// 事件总线功能自检（Day5）：单线程发布两条事件，drain 后由订阅者累加，
// 校验"发布→drain→分发"全链路工作。被 skeleton 测试调用以证明事件总线可用。
bool event_bus_selfcheck() {
    EventBus<int> bus(1024);
    std::atomic<int> sum{0};
    bus.subscribe("selfcheck", [&](int v) { sum.fetch_add(v, std::memory_order_relaxed); });
    bus.publish("selfcheck", 7);
    bus.publish("selfcheck", 3);
    const std::size_t handled = bus.drain("selfcheck", 100);
    return handled == 2 && sum.load() == 10;
}

}  // namespace common
}  // namespace cami
