// server/dataservice/include/mmo/data/result_helpers.h
//
// 构造统一失败结果的本地辅助（TASK-026）。
//
// 仅封装 `core::Result<T>::Fail(core::Error(code, msg, domain::kData))`，
// 让各方法返回点保持简洁，并统一打上 data 域，便于上层按域聚合错误。
// 对 T=void 同样可用（Result<void>::Fail 接受 Error）。

#pragma once

#include <string_view>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"

namespace mmo::data {

/// 构造一个携带 data 域的失败 Result<T>。
template <typename T>
inline core::Result<T> fail(core::ErrorCode code, std::string_view msg) {
    return core::Result<T>::Fail(core::Error(code, msg, core::domain::kData));
}

}  // namespace mmo::data
