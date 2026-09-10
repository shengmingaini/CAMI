// server/dataservice/include/mmo/data/idata_store.h
//
// IDataStore —— 权威持久化存储接口（TASK-026）。
//
// 这是「外部持久化」的统一抽象：具体实现可以是关系型/列式/对象存储等，
// 但本任务只要求内存实现（InMemoryStore）用于无数据库启动与测试。
// 接口只管键值与版本，禁止混入任何业务语义（见任务书 §21）。
//
// 版本冲突：Save/Delete 带 VersionCheck，required=true 且期望版本 ≠ 实际版本
// 时返回 VERSION_CONFLICT，由调用方决定重试或放弃，禁止静默覆盖。
//
// 批量：BatchSave/BatchLoad 逐条返回结果，部分失败不得整批吞错。

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/data/record.h"

namespace mmo::data {

/// 批量写入中单条记录的结果（逐条透传，禁止整批吞错）。
struct BatchOutcome {
    DataKey key;             // 出问题的键
    bool ok{false};          // 本条是否成功
    std::uint32_t code{0};   // 失败时的错误码（core::ErrorCode 数值）
};

/// 权威持久化存储接口（对应外部关系型/对象存储角色）。
class IDataStore {
public:
    virtual ~IDataStore() = default;

    /// 按键加载单条；不存在返回 std::nullopt（Ok 包裹）。
    virtual core::Result<std::optional<Record>> Load(const DataKey& key) = 0;

    /// 写入单条；vc.required 且版本不符时返回 VERSION_CONFLICT。
    virtual core::Result<void> Save(const Record& rec, VersionCheck vc = {}) = 0;

    /// 删除单条；vc.required 且版本不符时返回 VERSION_CONFLICT。
    virtual core::Result<void> Delete(const DataKey& key, VersionCheck vc = {}) = 0;

    /// 批量加载：仅返回实际存在的记录（缺失键静默跳过）。
    virtual core::Result<std::vector<Record>> BatchLoad(std::span<const DataKey> keys) = 0;

    /// 批量写入：每条独立结算，逐条写入 BatchOutcome；部分失败整批仍返回 Ok(向量)。
    virtual core::Result<std::vector<BatchOutcome>> BatchSave(std::span<const Record> recs) = 0;
};

}  // namespace mmo::data
