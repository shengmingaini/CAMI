#pragma once
#include <cstddef>
#include <initializer_list>

namespace cami {
namespace data {

// 数据管理层 (spec §3.1)
inline constexpr char kLayerName[] = "data";
// 依赖：common (日志/监控等公共服务)
inline constexpr std::initializer_list<const char*> kDependsOn = {"common"};
inline constexpr std::size_t kDependsOnSize = 1;

// 链接锚点声明（定义见 .cpp），skeleton 自检调用以强制链接本层符号。
const char* layer_name();
const char* depends_on();

}  // namespace data
}  // namespace cami
