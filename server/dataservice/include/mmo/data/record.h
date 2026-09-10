// server/dataservice/include/mmo/data/record.h
//
// 数据记录与版本契约（TASK-026 · DataService Interface）。
//
// 设计要点：
//   - Record 是「序列化后字节 + 版本号 + 更新时间」的中性载体，不绑定任何具体
//     外部持久化/缓存技术；业务层负责把领域对象序列化进 payload。
//   - DataKey 采用 `<domain>:<id>` 命名（如 "character:10086" / "inventory:10086"），
//     由调用方保证唯一性。
//   - VersionCheck 是乐观锁：required=true 时，期望版本必须与实际版本一致，
//     否则 Save/Delete 返回 VERSION_CONFLICT（禁止静默覆盖）。

#pragma once

#include <cstdint>
#include <string>

#include "mmo/core/time/clock.h"

namespace mmo::data {

/// 数据键：`<domain>:<id>` 形式，全局唯一。
using DataKey = std::string;

/// 单条数据记录（与具体存储技术无关的中性载体）。
struct Record {
    DataKey key;                          // 主键
    std::string payload;                  // 序列化后的字节（业务层负责编解码）
    std::uint32_t version{0};            // 乐观锁版本号，每次成功写入递增
    mmo::core::SteadyTime updated_at{};   // 最后更新时刻（单调时钟，仅用于审计/调试）
};

/// 乐观锁版本检查。
struct VersionCheck {
    std::uint32_t expected_version{0};    // 期望的实际版本
    bool required{true};                  // true 时强制校验；false 时跳过校验直接覆盖
};

}  // namespace mmo::data
