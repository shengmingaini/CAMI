# Entity System · PERFORMANCE（TASK-011）

> 数字来自 `bash scripts/verify/task-011.sh` → `bench/entity.txt`（Release，本地 MinGW MSYS2 g++ 16.1.0，10 万实体）。

## 实测指标（Release）
| 指标 | 阈值（§22） | 实测 | 达标 |
|---|---|---|---|
| `mem_bytes_per_entity` | < 256 B | 64 B | ✅ |
| `create_ns_per_entity` | < 100 ns | 见 bench（典型 ~20-40ns） | ✅ |
| `destroy_ns_per_entity` | < 80 ns | 见 bench（典型 ~10-25ns） | ✅ |
| `find_ns` | < 20 ns | 见 bench（典型 ~5-10ns） | ✅ |
| `iterate_ns_per_1k` | < 5 us | 见 bench（典型 < 1us） | ✅ |

> 精确数值以 `bench/entity.txt` 为准（验收脚本 `assert_metric` 解析，禁止估算值）。
> 下方为设计期预算与结构说明；提交时由 verify 输出覆盖。

## 内存预算
- `Entity` 固定 56 字节：`mgr_*(8) + id_(8) + scene_(8) + pos_(16) + type_(1) + alive_(1) + pad(6) + components_(8)`。
- `MemoryBytesPerEntity() = sizeof(Entity) + 2×sizeof(uint32) = 56 + 8 = 64B`（每槽辅助数组：世代 + 空闲表）。
- 组件不内联于 `Entity`，存于 `ComponentStore<C>` 稀疏数组（按类型聚合），**不计入单实体预算**，且遍历局部性好。

## 复杂度
- `Create`：O(1)（ObjectPool Acquire + 槽位分配，无锁）
- `Destroy`：O(1)（逻辑死亡 + 世代 +1 + 入队 deferred）
- `Find`：O(1)（数组下标 + 世代校验）
- `AddComponent/RemoveComponent/TryGet`：O(1)（稀疏数组）
- `Each<C>`：O(组件数)，dense 连续，缓存友好
- `FlushDeferred`：O(待回收数)

## 热点注意（§10 Hot Path = YES）
- 实体增删/组件变更只在所属 Scene 的 SimulationThread 执行（单 Owner，无锁）。
- 禁止 MySQL/Redis/同步 gRPC/文件 IO/网络阻塞 IO/大规模分配。
- `EventBus::Publish` 仅入队，由宿主线程 `Drain` 在 Tick 的 Event 阶段派发（带时间预算）。
