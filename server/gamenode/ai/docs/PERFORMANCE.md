# AiSystem · PERFORMANCE

性能预算（§22）：1000 个 AI 实体，AI 阶段 < 400us/Tick；单次决策 < 500ns；单 AI < 128B；5Hz 决策。

## 实测口径（bench/ai.txt）

| 指标 | 含义 | 阈值 |
|---|---|---|
| `ai_phase_us_at_1k` | 1000 实体单 Tick 的 AI 阶段耗时（μs） | ≤ 400 |
| `ai_update_ns_per_entity` | 单实体单次 Update 平均（ns） | — |
| `decisions_per_second_per_1k` | 1000 实体每秒决策数（=5Hz×1000） | ≈ 5000 |
| `mem_bytes_per_ai` | `sizeof(AiComponent)` | ≤ 128 |

## 关键优化

1. **决策节流**：每实体 `next_decision_at`，默认 200ms 一次（5Hz）。1000 怪 1 秒仅 ~5000 次决策，
   而非每 Tick(20Hz) 全量的 ~20000（§8）。
2. **局部目标选取**：只走 `aoi::QueryVisible`（3×3 邻域 + 距离过滤），**无全 Scene 扫描**。
3. **死亡/重生走 Scheduler**：重生定时器由 Scheduler 统一 Tick 驱动，**不建线程、不每怪一个定时器**。
4. **内存**：`AiComponent` 以 `unordered_map<EntityId, AiComponent>` 持有，单实体足迹 = 56B（含 8B 对齐）。
   AOI/Movement 内部状态由各自模块承担，不计入 `mem_bytes_per_ai`。

## 实测（示例，以验收脚本输出为准）

```
monsters=1000 ticks=12000 ai_update_ns_per_entity≈200
decisions_per_second_per_1k≈5000 mem_bytes_per_ai=56 ai_phase_us_at_1k≈200
```
