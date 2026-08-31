# TASK-015 · Movement System — Performance Notes

> 验收阈值（TASK-015 §22 / §24）：`bench/movement.txt` 中 `move_ns_per_entity ≤ 500`。
> 实测数字见下表，由 `bin/movement_bench --entities <N> --ticks 10000` 产出（**Release** 构建）。

## 1. 四档基准（MSYS2 MinGW g++ 16.1.0，Ninja，Release，--ticks 10000）

| 实体数 | move_ns_per_entity | validate_ns | integrate_ns_per_1k | aoi_update_ns |
|-------:|-------------------:|------------:|--------------------:|--------------:|
| 100    | 34.661             | 4.400       | 3.000               | 1128.000      |
| 500    | 36.372             | 4.800       | 2.600               | 1325.200      |
| 1000   | 35.697             | 4.645       | 2.700               | 1615.100      |
| 2000   | 35.721             | 4.405       | 2.400               | 1551.350      |

> 阈值核对：`move_ns_per_entity(1000)=35.697 ≤ 500` ✓（**约 14 倍余量**）。
> 关键结论：`move_ns_per_entity` 在 100→2000 区间**保持平坦**（34.7–36.4ns），
> 证明单条命令的处理成本与实体总数**无关**（O(1) 哈希寻址，无全量扫描）。

## 2. 成本构成（§10 / §22）

| 项 | 说明 | 量级 |
|----|------|------|
| `ApplyCommand` 全链路 | 非有限检查 + `Find` + 校验 + 钳制 + 写实体 + 事件发布 | ~35ns |
| `ValidateMovement` 纯函数 | 6 条规则全跑（无状态突变、无分配、无事件） | ~4.5ns |
| `Integrate` 单实体 | 速度积分 + 边界钳制 + 写实体；零速度实体直接 `continue` 跳过 | ~2.7ns |
| AOI 联动（`IAoi::Move`） | **独立阶段成本**，见下节说明 | ~1.5μs |

`validate_ns ≈ 4.5ns` 说明校验本身只占 `ApplyCommand` 的 ~13%，
其余开销集中在实体查找（`EntityManager::Find` 哈希）与权威位置回写 —— 均属必要的 §27.2 跨模块调用。

## 3. AOI 联动成本为何单独计量（§15.6 / §22）

`aoi_update_ns ≈ 1.5μs/实体`，比移动本身高约 **40 倍**。这是 TASK-014 的固有成本
（TASK-014 实测 `query_ns_p99(1000)=1700ns`，与本模块测得的 1.5μs 属同一量级，**非本模块回归**）。

因此：

1. `move_ns_per_entity` 的 bench **刻意不绑定 AOI**，隔离测出移动阶段自身成本；
2. AOI 在 TASK-013 调度器中是**独立阶段**（八阶段之三），不占用 Movement 阶段预算；
3. 生产路径里 AOI 失败只计数（`aoi_errors`）**不回滚位置**（§19），故其耗时不构成移动正确性的阻塞项。

## 4. 热路径约束（§10 / §11 / §21）

- **零分配**：`ApplyCommand` / `Integrate` 全程不调用 `new` / `malloc`，
  事件经 `EventBus::Publish` 以内联值类型发布（`EntityMoved` 24B ≤ 32B 内联预算）。
- **无锁**：`states_` 为 `unordered_map`，单 Owner 模拟线程独占，无互斥（§9）。
- **无阻塞 IO**：无 DB / Redis / gRPC / 文件 / 网络调用（验收脚本 `scan_forbidden` 已扫描 `src/`）。
- **去重**：命令驱动过的实体置 `moved_tick = ctx.tick_number`，同 Tick 的 `Integrate` 跳过，
  杜绝「命令位移 + 速度积分」双计位移。

## 5. 正确性 / 反作弊实测（`movement_test`，8 函数全绿）

| 检查 | 实测 |
|------|------|
| 匀速积分精度 | 6m/s × 10s（200 Tick），位移误差 `1.068e-04 m`（« 1cm 阈值） |
| 1000 实体 × 300 Tick 随机移动 | 全部位置 finite 且在 `[-1e6, 1e6]` 内 |
| 作弊检出（10% 注入瞬移） | 29991 次作弊 → 29580 次 `Teleport` 拒绝 |
| 纠偏率 | 72 / 300000 次操作 ≈ 0.024%（« 5% 上限） |
| AOI 一致性 | 抽样实体 `QueryVisible` 与 O(N²) 暴力实现对拍，**完全一致** |

> 注：与 TASK-014 一致，Debug 构建（无优化）耗时显著更高，验收以 **Release** 为准。
