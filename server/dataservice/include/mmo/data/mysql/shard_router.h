// server/dataservice/include/mmo/data/mysql/shard_router.h
//
// TASK-028 · 分片路由（§15.2）。
//
// 约束（§21 Forbidden）：禁止把分片数写死到业务层。故本类的使用者只有 DataService
// 与仓储层；业务代码一律只传业务 ID，由本模块决定落到哪个分片。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/data/mysql/mysql_config.h"
#include "mmo/data/record.h"

namespace mmo::data::mysql {

/// 业务键解析结果：`<domain>:<id>`。
struct DataKeyParts {
    std::string domain;       // 逻辑域（对应表名前缀，如 "character"）
    std::uint64_t entity_id;  // 业务 ID（分片键）
};

class ShardRouter {
public:
    /// 构造：校验 shard_count 与 endpoints 数量一致、shard_count > 0。
    static core::Result<ShardRouter> Create(const ShardConfig& cfg,
                                            const std::vector<ShardEndpoint>& endpoints);

    /// 业务 ID -> 分片序号。id=0 与极大 id 均安全（§16 边界用例）。
    std::uint32_t ShardOf(std::uint64_t business_id) const noexcept;

    /// 分片端点；越界返回第一个端点的引用（并置 shard_valid=false）。
    const ShardEndpoint& EndpointOf(std::uint32_t shard) const noexcept;

    /// 越界查询诊断（配合 EndpointOf 使用，避免抛异常）。
    bool IsValidShard(std::uint32_t shard) const noexcept;

    /// 预留接口：第一版不执行数据迁移，仅校验计划一致性并返回明确错误。
    core::Result<void> Reshard(std::uint32_t new_count, const ReshardPlan& plan);

    std::size_t ShardCount() const noexcept { return endpoints_.size(); }

    /// 解析业务键 `<domain>:<id>`；格式非法返回 INVALID_ARGUMENT（禁止崩溃）。
    static core::Result<DataKeyParts> ParseKey(const mmo::data::DataKey& key);

private:
    ShardRouter(ShardConfig cfg, std::vector<ShardEndpoint> endpoints);

    ShardConfig cfg_;
    std::vector<ShardEndpoint> endpoints_;
};

}  // namespace mmo::data::mysql
