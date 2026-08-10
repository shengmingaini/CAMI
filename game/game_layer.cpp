#include "game/game_layer.h"

namespace cami {
namespace game {

const char* layer_name() { return kLayerName; }
const char* depends_on() { return kDependsOnSize ? *kDependsOn.begin() : "none"; }

}  // namespace game
}  // namespace cami
