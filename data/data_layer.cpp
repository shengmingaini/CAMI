#include "data/data_layer.h"

namespace cami {
namespace data {

const char* layer_name() { return kLayerName; }
const char* depends_on() { return kDependsOnSize ? *kDependsOn.begin() : "none"; }

}  // namespace data
}  // namespace cami
