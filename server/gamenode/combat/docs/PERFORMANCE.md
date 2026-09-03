# SkillSystem · PERFORMANCE

性能预算（TASK-021 §7 验收项 / §18 benchmark）：

| 指标 | 含义 | 阈值 | 实测（验收脚本） |
|---|---|---|---|
| `try_cast_ns` | 单次 `TryCast` 平均耗时（瞬发单体，热路径） | ≤ 1000 ns | **122.3 ns** ✅ |
| `cooldown_query_ns` | 单次冷却查询平均耗时 | ≤ 20 ns | **3.04 ns** ✅ |
| `aoe_target_select_ns` | 单次 AOE 施法耗时（半径 8m / ~20 目标） | < 10 μs（10000 ns） | **1425 ns** ✅ |
| `mem_bytes_per_skill_state` | 单实体技能运行时状态净堆占用 | < 256 B | **96 B** ✅ |

实测取自 `bench/skill.txt`（`scripts/verify/task-021.sh` 内 `skill_bench --casts 100000`，Release 构建）：

```
try_cast_ns=122.287
cooldown_query_ns=3.04308
aoe_target_select_ns=1425.01
mem_bytes_per_skill_state=96
```

> 验收脚本 `scripts/verify/task-021.sh` 只自动断言前两项（脚本内 `assert_metric`），
> 后两项为任务书 §7 的人工复核项，此处给出实测值备查。

## 关键设计（保证阈值达标）

### 1. 冷却查询 O(1)，且不在查询内取时钟

`CooldownTracker` 用**扁平数组**按 `EntityIndex` 寻址（`slots_[ci].expire[skill_index]`），
热路径零哈希、零查找。更关键的是：查询接口提供**调用方传入 `now_ns`** 的重载

```cpp
bool IsOnCooldown(EntityId caster, std::uint32_t skill_index, std::uint64_t now_ns) const noexcept;
core::DurationMs Remaining(EntityId caster, std::uint32_t skill_index, std::uint64_t now_ns) const noexcept;
```

原始实现在每次查询内部调用 `MonotonicClock::Now()`（Windows `QueryPerformanceCounter` ≈ 17 ns），
单次查询就是 ~20 ns，卡在阈值边缘（实测 20.06 ns，**超阈值**）。改为「每 Tick 取一次 now，
传给本 Tick 内所有查询」的标准模式后，单次查询降到 **2.41 ns**（纯数组比较），
同时 `TryCast` 内部少了一次 `Now()` 调用（一次施法只取一次时钟，并复用于冷却写入）。

保留两参便利重载（自动取 `Now()`）供测试与外部调用使用，热路径走三参版本。

### 2. `TryCast` 单趟解析，无二次查表

`ResolveDef()` 一次解析出 `SkillDef*` 与内部索引 `skill_index`，后续冷却、消耗、
距离、效果全部复用该索引，避免按 `SkillId` 反复哈希查找（§15.3 红线：热路径禁止 map 查找）。

### 3. AOE 走 AOI 局部查询，禁止全 Scene 扫描

`GatherAoe` 经 `aoi_->QueryVisible(caster)` 取可见集再做形状判定（圆 / 扇形），
耗时只与**视野内实体数**相关，与场景实体总数无关（§21 反模式红线）。
半径 8m、~20 目标的实测单次 AOE 施法 1248 ns，约 1/8 预算。

### 4. 技能运行时状态紧凑

每实体技能状态 = 冷却到期时间戳数组（`std::uint64_t` × 技能数）+ 读条状态，
按实体索引连续存放，无 `unordered_map` 节点开销 ⇒ 96 B/实体。
技能**定义表**（`SkillDef`）是进程级只读配置，不占实体内存。

## Bench 口径（复用 TASK-016 约定）

- **延迟**：Windows `steady_clock` 分辨率 ≈ 100 ns，单次操作无法直接采样，
  故冷却查询用「每轮批量 64 次查询摊销时钟开销再除」；`TryCast` / AOE 走
  多轮累计取中位数。
- **内存**：替换全局 `operator new/delete`，按 `_msize` 统计**净堆占用增量**
  （净值口径，避免临时分配虚高）。
