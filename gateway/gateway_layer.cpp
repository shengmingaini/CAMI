#include "gateway/gateway_layer.h"

namespace cami {
namespace gateway {

const char* layer_name() { return kLayerName; }
const char* depends_on() { return kDependsOnSize ? *kDependsOn.begin() : "none"; }

}  // namespace gateway
}  // namespace cami
