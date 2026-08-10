#include "ops/ops_layer.h"

namespace cami {
namespace ops {

const char* layer_name() { return kLayerName; }
const char* depends_on() { return kDependsOnSize ? *kDependsOn.begin() : "none"; }

}  // namespace ops
}  // namespace cami
