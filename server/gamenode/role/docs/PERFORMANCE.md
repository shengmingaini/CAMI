# TASK-016 · Player / Character — Performance

> 数据来源：`bin/role_bench --characters 1000`（Release，MinGW MSYS2 g++ 16.1.0，Ninja）。
> 输出落 `bench/role.txt`，验收脚本 `assert_metric` 直接解析该文件。

## 1. 实测结果（`--characters 1000`，iters=200000）

| 指标 | 实测 | §22 预算 | 结论 |
|---|---|---|---|
| `recompute_ns` | **22.600** | ≤ 300 | ✅ 余量 13× |
| `add_exp_ns` | **36.100** | ≤ 100 | ✅ |
| `modify_hp_ns` | **42.300** | ≤ 50 | ✅（余量 16%） |
| `save_enqueue_ns` | **5.400** | ≤ 200 | ✅ 余量 37× |
| `mem_bytes_per_character` | **496.000** | ≤ 512 | ✅（余量 16B） |

同轮 p99（bench 侧排序，不进热路径）：`recompute=32.8 / add_exp=41.6 / modify_hp=55.2 / save=16.6`。

## 2. 计量口径

**延迟**：Windows `steady_clock` 分辨率约 100ns，单次操作（几十 ns）无法直接采样。
改为「批量计时 + 除法」：每轮遍历全部 N 个角色各操作一次，累计 R 轮后取
`总时间 / (N * R)`，再对 R 个轮次样本取中位数，规避调度抖动。
`add_exp` 循环额外缓存 `Character*` 指针数组（`Reserve` 后容器不 rehash，指针稳定），
避免把「把 exp 归零」的哈希查找算进 `AddExp` 本身。

**内存**：替换全局 `operator new/delete`，按 `_msize` 统计**净堆占用**增量。
用净值而非「累计分配」的原因：`LoadOrCreate` 内部有 `std::to_string` 之类的临时分配，
累计口径会把它们算成每角色常驻成本，虚高且不可复现。

**496B/角色**的构成（`--characters 1000`）：

| 组成 | 约 | 说明 |
|---|---|---|
| `unordered_map` 节点（key 8B + `Character` 456B + next 8B） | ~480B | `sizeof(Character)=456`，含 `std::string name`（32B，短名 SSO 不额外分配） |
| `chars_` bucket 摊销 | ~8B | `Reserve(1000)` 后一次性分配，按 N 摊销 |
| `by_player_` 节点 + bucket | ~8B | `pair<const PlayerId, CharacterId>` 16B，同样摊销 |

`AttributeSet` 固定 352B（`static_assert`），钳制区间与派生公式是**进程级**配置，不占角色内存。

## 3. 热路径成本构成

- `ModifyHp`（42.3ns）：哈希查找 ≈ 15ns + `HpChanged` 事件入队 ≈ 20ns + 钳制/版本递增 ≈ 7ns。
  事件发布是最大项，但 `HpChanged` 是 TASK-022 伤害系统的契约，不可省略。
- `AddExp`（36.1ns）：哈希查找 + 溢出检测 + 升级循环（不升级时为常数）。
  升级路径额外付一次 `AttributeSet::Recompute()`（22.6ns）。
- `Recompute`（22.6ns）：11 个属性，每属性 3 层求和 + 钳制；派生属性按 `AttrFormula` 一次乘加。
  **无排序、无分配、无虚调用**。
- `Save`（5.4ns）：一次哈希查找 + `EnqueueSave` 虚调用 + `id` 入 `vector`。

## 4. 集成测试观测（§17）

`role_test` 的 `TestIntegration1000`：1000 角色 × 1000 Tick，每 Tick 100 次 `AddExp(1)`
+ 100 次 `ModifyHp(-1)`，每 100 Tick 对**全部 1000 个角色**批量存档。

```
tick_us p50=8.2 p99=13.9 | save_tick_us worst=21.0 (batches=10)
level_ups=523 deaths=0 save_enqueued=10000
```

含 1000 次 `Save` 的 Tick 最差 **21.0μs**，对比普通 Tick p99 13.9μs —— 存档只入队不等待落盘，
对 Tick 无可观测阻塞（§17 / 验收项 5）。
