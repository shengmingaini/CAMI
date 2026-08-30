// TASK-007 · bus 测试夹具：共享的 Command / Query / Event 值类型。
//
// 设计约束（来自 event_slot.h 的内存预算）：
//   - 事件必须保持小值类型（<=32B）才能走内联路径、做到 Publish 零堆分配；
//   - 命令是同步执行的，不入队，因此大小不受限（但同样建议保持值语义）。

#ifndef MMO_CORE_TESTS_BUS_FIXTURES_H
#define MMO_CORE_TESTS_BUS_FIXTURES_H

#include <cstdint>

#include "mmo/core/bus/command.h"

namespace mmo::core::bus_fixtures {

struct Position {
    float x{0.0f};
    float y{0.0f};
};

// ---- Command：移动玩家（§8 要求的 5 个审计字段齐全） ----------------------

struct MovePlayerCommand {
    using Result = Position;
    RequestID request_id{0};
    PlayerID player_id{0};
    CommandSource source{CommandSource::kInternal};
    std::int64_t timestamp{0};
    std::uint32_t version{1};
    float dx{0.0f};
    float dy{0.0f};
};
static_assert(CommandLike<MovePlayerCommand>, "MovePlayerCommand must satisfy CommandLike");

// ---- Query：只读查询位置 --------------------------------------------------

struct GetPositionQuery {
    using Result = Position;
    RequestID request_id{0};
    PlayerID player_id{0};
};
static_assert(QueryLike<GetPositionQuery>, "GetPositionQuery must satisfy QueryLike");

/// 未注册的查询类型：用于断言 NOT_FOUND。
struct UnregisteredQuery {
    using Result = int;
    RequestID request_id{0};
};

// ---- Event：已发生的事实 --------------------------------------------------

/// 移动完成事件：正好 32 字节，走内联路径（零堆分配）。
struct PlayerMovedEvent {
    PlayerID player_id{0};
    SceneID scene_id{0};
    std::int32_t x{0};
    std::int32_t y{0};
    RequestID request_id{0};  // 串联 Command 与 Event，供 Demo 断言
};
static_assert(sizeof(PlayerMovedEvent) <= 32, "keep events <=32B to stay on the inline path");

/// 经济类事件：**关键事件**，队列满时禁止丢弃（§21）。
struct EconomyEvent {
    static constexpr bool kCritical = true;
    PlayerID player_id{0};
    std::int64_t delta{0};
    std::uint32_t item_id{0};
};
static_assert(sizeof(EconomyEvent) <= 32, "keep events <=32B to stay on the inline path");

/// 大事件（>32B）：走堆分配路径，用于验证 inline / heap 两条路径行为一致。
struct BigEvent {
    PlayerID player_id{0};
    std::int64_t payload[4]{0, 0, 0, 0};
};
static_assert(sizeof(BigEvent) > 32, "BigEvent is meant to exercise the heap path");

}  // namespace mmo::core::bus_fixtures

#endif  // MMO_CORE_TESTS_BUS_FIXTURES_H
