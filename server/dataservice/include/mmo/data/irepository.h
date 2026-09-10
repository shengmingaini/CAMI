// server/dataservice/include/mmo/data/irepository.h
//
// IRepository<T> —— 面向领域对象的泛型仓储接口（TASK-026）。
//
// 建立在 IDataStore 之上：T 是业务领域对象（角色/背包/任务等），
// 由具体仓储实现负责 T <-> Record 的编解码。本任务只定义契约，
// 具体仓储留给后续业务任务实现。

#pragma once

#include <cstdint>
#include <optional>

#include "mmo/core/error/result.h"
#include "mmo/data/record.h"

namespace mmo::data {

/// 领域对象泛型仓储接口（T 为业务对象类型）。
template <typename T>
class IRepository {
public:
    virtual ~IRepository() = default;

    /// 按业务 id 加载；不存在返回 std::nullopt（Ok 包裹）。
    virtual core::Result<std::optional<T>> GetById(std::uint64_t id) = 0;

    /// 写入（可带乐观锁）。
    virtual core::Result<void> Put(const T& entity, VersionCheck vc = {}) = 0;

    /// 删除（可带乐观锁）。
    virtual core::Result<void> Remove(std::uint64_t id) = 0;
};

}  // namespace mmo::data
