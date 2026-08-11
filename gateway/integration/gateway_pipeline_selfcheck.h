#pragma once

namespace cami {
namespace gateway {
namespace integration {

// 集成层自检：组合四模块的缝，确定性验证（无 GTest / 无真实 socket）。
// 返回约定与兄弟模块一致：true = 成功。
bool gateway_pipeline_selfcheck();

}  // namespace integration
}  // namespace gateway
}  // namespace cami
