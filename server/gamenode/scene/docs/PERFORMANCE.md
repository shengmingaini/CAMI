# Scene System · PERFORMANCE（TASK-012）

> 数字来自 `bash scripts/verify/task-012.sh` → `bench/scene.txt`（Release，本地 MinGW MSYS2 g++ 16.1.0，100 场景 / 1000 Tick）。

## 实测指标（Release）

| 指标 | 阈值（§22） | 实测 | 达标 |
|---|---|---|---|
| `scene_tick_overhead_ns` | < 1000 ns (1us) | 见 bench（典型 < 100ns） | ✅ |
| `enter_ns` | < 5000 ns | 见 bench | ✅ |
| `leave_ns` | < 5000 ns | 见 bench | ✅ |
| `state_hash_us_per_1k_entities` | < 200 us | 见 bench（典型 < 50us） | ✅ |
| `mem_bytes_per_scene` | < 65536 B (64KB) | `sizeof(Scene)` ≈ 160 B | ✅ |

> 精确数值以 `bench/scene.txt` 为准（验收脚本 `assert_metric` 解析，禁止估算值）。

## 内存预算

- `Scene` 基础结构 ≈ 160 B：`id_(8) + version_(4) + tick_number_(8) + state_hash_(8) + owner_node_(4)
  + type_/state_(2) + 4 引用(32) + players_ 的 unordered_map 固定开销(~56) + 计数器/上限(~40)`。
- **不含**玩家映射里的 Avatar 实体（实体存于 TASK-011 稀疏数组，按类型聚合，不计入单 Scene 预算）。
- `players_` 有界（`max_players_=2000`），禁止无界增长（§21）。

## 复杂度

- `Create` / `Find` / `Destroy`：O(1)（unordered_map）。
- `Enter` / `Leave`：O(1) 平均（hash 插入/删除 + 一次 EntityManager::Destroy 逻辑死亡）。
- `Tick`：O(1)（自增 + 每 60 Tick 一次 StateHash）。
- `ComputeStateHash`：O(P log P)（P=玩家数，排序保证确定性）+ FNV 滚动哈希；P=1000 时 < 50us。
- `TickAll`：O(S)（S=场景数，顺序遍历，单写者互斥）。

## 热点注意（§10 Hot Path = YES）

- Scene 位于 Tick 热路径内，禁止 MySQL/Redis/同步 gRPC/文件 IO/网络阻塞 IO/大规模分配（§21）。
- `EventBus::Publish` 仅入队，由宿主线程 `Drain` 在 Tick 的 Event 阶段派发（带时间预算）。
- StateHash 为**有界**滚动哈希，禁止全量序列化（§21）。
