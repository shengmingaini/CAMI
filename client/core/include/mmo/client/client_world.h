#pragma once

/// TASK-034 · 客户端世界镜像 + 插值（§13 / §14）。
///
/// 设计要点：
///   - 服务端权威，客户端只持镜像：每个实体保留最近两段快照，渲染时做插值。
///   - 缓冲 100ms（render_time = now - 100ms），在两段快照间线性插值；
///     alpha 超出 [0,1] 时钳制（防止回溯/跳变）。
///   - 外推上限 200ms：若最新快照比 render_time 旧超过 200ms（即网络停滞），
///     冻结在最新姿态（不继续外推，避免「飘移」），仅错误计数 +1。
///   - 快照编解码为自包含二进制（WorldSnapshot 内部表示），与协议层 fbs 解耦；
///     GDExtension / 传输层负责把 TASK-005 AOI 格式映射进本结构（不变项：AOI 格式）。
///   - 不依赖服务端模块。

#include "mmo/client/types.h"

#include <cstdint>
#include <map>
#include <vector>

namespace mmo { namespace client {

/// 单个实体的渲染态（插值结果）。
struct EntityRender {
    EntityId id = 0;
    Vec3     pos{};
    float    heading = 0.0f;
    bool     frozen = false;  // 超过外推上限 -> 冻结
};

class ClientWorld {
public:
    /// 应用一帧世界快照（整帧或 AOI 增量累积后的结果）。
    void ApplySnapshot(const WorldSnapshot& snap);

    /// 由快照序列编码（供测试 / 网络载荷）。
    static std::vector<std::uint8_t> EncodeSnapshot(const WorldSnapshot& snap);
    /// 解码（与 EncodeSnapshot 对称）。
    static WorldSnapshot DecodeSnapshot(std::string_view bytes);

    /// 渲染时插值。render_time_ms 为服务器时间轴（通常 = 本地单调 - 缓冲）。
    /// 返回当前所有实体的渲染态。
    std::vector<EntityRender> Interpolate(std::int64_t render_time_ms,
                                          std::int64_t extrapolation_limit_ms = 200);

    /// 插值缓冲（默认 100ms）：渲染比最新快照「落后」该时长，换取平滑。
    std::int64_t interp_buffer_ms = 100;

    std::size_t EntityCount() const noexcept { return entities_.size(); }

private:
    struct EntityState {
        std::int64_t last_ts = 0;
        EntityPose   prev;
        EntityPose   curr;
        bool         has_prev = false;
        bool         has_curr = false;
    };
    std::map<EntityId, EntityState> entities_;
    std::uint64_t frozen_count_ = 0;
};

}}  // namespace mmo::client
