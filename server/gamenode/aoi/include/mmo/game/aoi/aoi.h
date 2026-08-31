#pragma once

/// TASK-014 · AOI System（Dynamic Grid 第一版）公开接口（§7）。
///
/// 命名空间：mmo::game::aoi。
/// 五个统一接口：Enter / Leave / Move / QueryVisible / Broadcast。
/// 算法：Dynamic Grid（稀疏格子哈希 + 3×3（或 ceil(view_radius/cell_size)）邻域扫描）。
///
/// 类型说明：任务书 §7 写作 `entity::EntityId` / `Position`，实际类型定义在 TASK-011 实体模块
/// 的 `mmo::game` 命名空间，此处用类型别名对齐（禁止重复定义第二套实体类型，见 TASK-012 §27.2）。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity.h"

namespace mmo::game::aoi {

// 复用 TASK-011 实体模块的实体标识与位置类型（任务书简写为 entity::EntityId / Position）。
using EntityId = mmo::game::EntityId;
using Position = mmo::game::Position;

/// AOI 配置（§7）。
struct AoiConfig {
    float   cell_size{20.0f};        // 格子边长（米）
    float   view_radius{50.0f};      // 视距（米）
    size_t  max_entities{10000};      // 容量上限，超出 Enter 返回 BUSY（§19 / §21）
    bool    use_dynamic_grid{true};   // 第一版固定 Dynamic Grid（保留扩展位）
};

/// Move 增量结果（§7）：进入/离开本实体视野的观察者集合 + 涉及的格子数。
struct MoveResult {
    std::vector<EntityId> entered;   // 移动后新进入视野的实体
    std::vector<EntityId> left;      // 移动后离开视野的实体
    std::uint32_t         touched_cells{0}; // 本次 Move 涉及（扫描）的格子数
};

/// AOI 运行期统计（§7 / §8）。
struct AoiStats {
    std::size_t entity_count{0};      // 当前实体数
    std::size_t cell_count{0};        // 当前格子数（稀疏）
    std::size_t avg_visible{0};       // 平均可见集大小（字节对齐用 size_t 存整数均值）
    std::uint64_t last_query_ns{0};   // 最近一次 QueryVisible 耗时（ns）
    std::uint64_t last_broadcast_ns{0}; // 最近一次 Broadcast 耗时（ns）
    std::size_t query_count{0};       // 累计查询次数
};

/// AOI 统一接口（§7，冻结契约）。
class IAoi {
public:
    virtual ~IAoi() = default;

    /// 实体进入 AOI（插入格子 + 建立初始可见关系）。
    /// 重复 Enter 同 id → INVALID_ARGUMENT；NaN/越界坐标 → INVALID_ARGUMENT；超容量 → BUSY。
    virtual core::Result<void> Enter(EntityId, const Position&) = 0;

    /// 实体离开 AOI（移除 + 解除可见关系）。不存在 → NOT_FOUND。
    virtual core::Result<void> Leave(EntityId) = 0;

    /// 移动实体到新位置，返回进入/离开的观察者集合（增量 diff，禁止全量重算）。
    /// 不存在 → NOT_FOUND；NaN/越界 → INVALID_ARGUMENT。
    virtual core::Result<MoveResult> Move(EntityId, const Position& to) = 0;

    /// 查询某实体当前可见集（3×3 邻域 + 距离过滤，去重）。不存在 → NOT_FOUND；out 由调用方提供。
    virtual core::Result<void> QueryVisible(EntityId, std::vector<EntityId>& out) const = 0;

    /// 向可见集广播 payload（复用内部缓冲区，禁止每目标单独分配/序列化）。返回送达目标数。
    /// 不存在 → NOT_FOUND。
    virtual core::Result<std::size_t> Broadcast(EntityId, std::span<const std::uint8_t> payload) = 0;

    /// 运行期统计快照。
    virtual AoiStats Stats() const noexcept = 0;
};

/// 工厂（§7，冻结签名）：仅以 AoiConfig 构造，无外部依赖（EventBus/传输由扩展点另行注入）。
std::unique_ptr<IAoi> CreateDynamicGridAoi(AoiConfig config = {});

}  // namespace mmo::game::aoi
