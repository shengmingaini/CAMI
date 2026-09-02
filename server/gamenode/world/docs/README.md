# World / Instance（TASK-020）

MMORPG 服务端 GameNode 内的世界与副本生命周期管理模块。WorldManager 管理 OpenWorld 常驻
场景与分线；InstanceManager 管理 Dungeon / Arena / Battleground 等副本实例的五状态机与回收。

## 设计要点

- **五状态机**：`Pending → Loading → Running → Completed → Destroying`。Loading 阶段由
  InstanceManager 创建 Scene 并通过 `ai::AiSystem::Spawn` 生成怪物/NPC；Running 阶段玩家可进入。
- **三条回收路径**（§8 / §15.4）：
  1. 超时（`time_limit` 到期）→ Completed → 宽限后 Destroying；
  2. 空实例（创建后从未进入 > `empty_instance_timeout`）→ 立即 Destroying；
  3. 全员退出（成员清空）→ 延迟 `destroy_grace` 秒 Destroying（防误杀）。
- **批量回收上限 10 / Tick**：100 个实例同时超时也不会产生 Tick 尖峰（§19 / §21 Forbidden）。
- **分线**：OpenWorld 单 Scene 玩家数超 `sharding_threshold`（默认 300）自动开分线，分线间互不干扰。
- **TransferPlayer**：走 Scene `Enter/Leave`，**禁止直接搬移实体**（§21 Forbidden）；目标 Scene
  已满返回 `BUSY`，玩家留在原场景。
- **全配置化**：实例/世界定义来自 `config/gameplay/world/*.json`，代码零硬编码（§15.1 / §21）。

## 依赖（§27.2，仅消费公开接口）

- `TASK-012` SceneManager / SceneContext
- `TASK-018` AiSystem（Loading 阶段生成怪物）
- `TASK-011` EntityManager（Avatar / 怪物实体）
- `TASK-004` core::{EventBus, Scheduler, Arena}

## 目录结构

```
server/gamenode/world/
├── include/mmo/game/world/   # 公开头（instance_def / instance_manager / world_manager / world_config）
├── src/                      # 实现（instance_manager / world_manager / world_config）
├── tests/world_test.cpp      # 单元 / 集成 / 失败测试（ctest -R World）
├── benchmark/world_bench.cpp # 性能基准（输出 bench/world.txt）
├── docs/                     # INTERFACE.md / README.md
└── CMakeLists.txt
```

## 构建与验收

```bash
# 本地 MinGW + vcpkg 双构建 + 测试 + 基准 + 指标断言
bash scripts/verify/task-020.sh
```

指标阈值（§22 / §24）：`mem_bytes_per_instance ≤ 4096`、`tick_us_per_100_instances ≤ 100`。
实例创建 < 5ms、销毁 < 3ms（§22，bench 实测记录）。

## 使用示意

```cpp
WorldManager world(scenes, instances, mgr, owner_node);
world.Init(WorldConfig{});
world.LoadConfig("config/gameplay/world");

// 开分线
auto shard = world.GetOrCreateOpenWorld(1);

// 组队进本
auto id = instances.Create(1001, {p1, p2, p3, p4, p5}, trace);
instances.Start(id, trace);                 // Pending→Loading→Running
instances.AddMember(id, p6, trace);         // 上限内可加
world.Tick(now);                            // 驱动 Scene + 回收
```
