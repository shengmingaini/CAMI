# TASK-015 · Movement System — Interface

> 冻结契约（§7）：本文件导出自 `server/gamenode/movement/include/`，签名一旦 `STATUS: DONE` 即契约冻结。

## 1. 命名空间与头文件

- 命名空间：`mmo::game::movement`
- 公开头：`include/mmo/game/movement/{movement_state.h, validator.h, movement_system.h}`
- 类型别名对齐 TASK-011：`EntityId` / `Position` 复用 `mmo::game`（禁止第二套实体类型）。

## 2. 核心类型（movement_state.h）

- `Vec3 {x,y,z}`：本模块自有三维速度向量（core / entity 未提供）。
- `MoveReject`：`None / TooFast / Teleport / OutOfBounds / NotMovable / RateLimited`。
- `MovementState`：权威坐标 `pos`、速度 `velocity`、最大速度 `max_speed`、状态标志 `move_flags`、客户端记账（`last_client_seq` / `last_client_timestamp_ms` / `last_client_update`）、去重 `moved_tick`。
- `MoveCommand`：客户端上行移动命令（`entity / from / to / request_id / client_timestamp_ms / client_seq`）。`from`/`to` 是**意图**，非权威。
- `MovementStats`：`moved_count` / `corrections` / `total_commands` / `aoi_errors` / `rejected_by_reason[6]`（反作弊指标）。
- `MovementConfig`：`max_speed` / `world_half` / `tick_dt` / `correction_threshold` / `max_commands_per_sec` / `stop_timeout_sec` / `speed_tolerance` / `teleport_factor`。

## 3. MovementSystem（movement_system.h）

```cpp
class MovementSystem {
    explicit MovementSystem(MovementConfig cfg = {}, aoi::IAoi* aoi = nullptr) noexcept;
    void BindAoi(aoi::IAoi&) noexcept;
    void UnbindAoi() noexcept;

    // 注意：SceneContext 定义在 namespace mmo::game（TASK-012），
    // 不存在 mmo::game::scene 命名空间，故此处为无限定名 SceneContext。
    core::Result<void> ApplyCommand(const MoveCommand&, const SceneContext&);
    core::Result<void> Integrate(const SceneContext&, float dt_seconds);
    core::Result<MoveReject> Validate(const MoveCommand&, const MovementState&) const noexcept;

    core::Result<void> SetSpeed(EntityId, float);
    core::Result<void> Stop(EntityId);

    core::Result<void> Register(EntityId, const MovementState&);
    // 返回指针而非引用：core::Result<T> 内部为 std::variant<T, Error>，不支持引用类型。
    core::Result<MovementState*> StateOf(EntityId);
    core::Result<void> SetVelocity(EntityId, Vec3);
    MovementStats Stats() const noexcept;
    void ResetStats() noexcept;
    std::size_t EntityCount() const noexcept;
    const MovementConfig& GetConfig() const noexcept;
};
```

## 4. 状态归属与权威模型（§4 / §21）

- 玩家坐标 (x,y,z) 与朝向的唯一权威写入者是 MovementSystem，仅在 Movement 阶段写入。
- 位移原点采用服务端权威 `MovementState.pos`，**不信任客户端 `from`**（防作弊）。
- 速度由服务端属性（`max_speed` / `velocity`）决定，**禁止采用客户端速度字段**。
- 校验（防加速 / 防穿墙）在写入点完成；所有拒绝计数上报 `rejected_by_reason`。

## 5. 校验规则（validator.h，纯函数 `ValidateMovement`，§8）

| 规则 | 判定 | 处理 |
|---|---|---|
| 速度上限 | `dist(pos,to) > max_speed*dt*1.15` | `TooFast`：钳制到合法位置后接受 + 计数 |
| 瞬移检测 | `dist(pos,to) > max_speed*dt*3` | `Teleport`：拒绝 + 计数 |
| 世界边界 | 有限越界 | `OutOfBounds`：钳制到边界后接受 + 计数 |
| 非有限坐标 | NaN / Inf / `\|coord\|>world_half*1e9` | `OutOfBounds`：拒绝，保持原位置 |
| 状态限制 | 眩晕/定身/死亡 | `NotMovable`：拒绝 |
| 频率 / 乱序 / 重放 | 时间戳间隔 `<1/max_cps` 或 `client_seq` 非单调递增 | `RateLimited`：拒绝 |

`ApplyCommand` 对 `Teleport`/`NotMovable`/`RateLimited`/非有限坐标返回 `Fail`（位置不变）；
对 `TooFast`/`OutOfBounds` 钳制后接受并更新权威位置。

## 6. AOI 联动（§15.6）

位置变化后调用 `aoi::IAoi::Move(id, pos)`，把 `MoveResult.entered/left` 转 `EntityViewEnter` /
`EntityViewLeave` 事件经 `SceneContext.events` 发布（§7 事件值类型 ≤ 32B）。AOI 失败（`Move` 返回
错误）只计数 `aoi_errors`，**位置不回滚**（§19）。

## 7. Tick 集成与去重（§9 / §15.4）

- `Integrate`：按 `velocity` 推进 `pos += velocity*dt`，钳制世界边界；无速度实体跳过。
- 外推超时：`now - last_client_update > stop_timeout_sec` → 清零速度（停止）。
- 去重：命令驱动的实体置 `moved_tick = ctx.tick_number`，`Integrate` 跳过同 Tick 已命令驱动的实体，
  避免与 `ApplyCommand` 双计位移；仅带残余速度（动量 / 外推）的实体由 `Integrate` 推进。

## 8. 模块边界（§27）

- 消费 TASK-011 `EntityManager`、TASK-012 `SceneContext`、TASK-014 `aoi::IAoi` 的**公开接口**。
- 禁止 `#include` 依赖模块 `src/`；禁止访问其内部数据；不引入数据库 / Redis / gRPC / 网络 IO。
