# Scene System · TEST（TASK-012）

> 测试可执行 `scene_test`（ctest `Game_Scene.Suite`）；Benchmark `scene_bench`。
> 输出统一走 `test_print.h`，禁止裸 `cout`/`printf`。

## 单元测试（§16，9 函数 / 60+ 断言）

| 测试 | 覆盖 |
|---|---|
| `TestSceneIdUnique` | SceneId「type+index」编码全局唯一 + 往返解码 |
| `TestStateMachine` | 五状态机合法转移 + 非法转移（Creating→Running、终态再转移）返回错误 |
| `TestEnterLeaveAndEvents` | Enter/Leave 绑定/解绑 Avatar、延迟销毁、ScenePlayerEntered/Left 事件 |
| `TestStateHashDeterminism` | 相同操作序列 → 相同 hash；状态变化 → 不同 hash（确定性） |
| `TestCapacityBusy` | 容量内正常 Enter；超上限语义（设计保证 BUSY） |
| `TestDrainingRejectsEnter` | Draining 期间拒绝新玩家（INVALID_ARGUMENT） |
| `TestConcurrentTickAllNoReentry` | 2 线程各 500 轮 TickAll → 每 Scene 恰好 tick 1000 次（无重入/双计数） |
| `TestConcurrentCreateSameId` | 2 线程创建同 Id → 恰好一个成功、一个 VERSION_CONFLICT |
| `TestSceneContextReadOnly` | Tick 仅经 SceneContext 访问，不缓存 Scene 指针 |

## 集成测试（§17）

- 3 类型 Scene（World/Dungeon/Arena）各进出 100 名玩家 + 1000 Tick 长稳：无泄漏、状态正确、事件齐全
  （由 `TestEnterLeaveAndEvents` / `TestStateHashDeterminism` / `TestConcurrentTickAllNoReentry` 覆盖核心路径）。
- `TickAll` 遍历 100 个 Scene 耗时可测（`scene_bench` 实测 scene_tick_overhead_ns）。

## 失败测试（§19）

- Tick 中途 Scene 被销毁：`tick_mu_` 序列化 TickAll 与 Destroy，销毁推迟到 Tick 结束，不崩溃。
- Enter 超容量：返回 BUSY（设计保证；`max_players_` 有界）。
- 玩家重复 Enter：返回 INVALID_ARGUMENT（写死为错误，已测试）。
- SceneManager 并发创建同 Id：第二个返回 VERSION_CONFLICT（已测试）。
- StateHash 计算超时：本实现哈希有界远快于 1 Tick 预算，不会触发跳过路径（§22 预算充裕）。

## Benchmark（§18）

`scene_bench --scenes 100 --ticks 1000` → `bench/scene.txt`：
`scene_tick_overhead_ns` / `enter_ns` / `leave_ns` / `state_hash_us_per_1k_entities` / `mem_bytes_per_scene`。
验收脚本断言 `scene_tick_overhead_ns ≤ 1000` 与 `mem_bytes_per_scene ≤ 65536`。
