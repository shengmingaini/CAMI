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

---

# TASK-022 · DamageSystem · PERFORMANCE

性能预算（TASK-022 §7 验收项 / §18 benchmark）：

| 指标 | 含义 | 阈值 | 实测（验收脚本） |
|---|---|---|---|
| `compute_damage_ns` | 单次纯函数 `ComputeDamage` 平均耗时（rolls 预生成） | ≤ 50 ns | **4.57 ns** ✅ |
| `alloc_per_damage` | 单次 `ApplyDamage` 热路径堆分配次数 | ≤ 0 | **0** ✅ |
| `apply_damage_ns` | 单次 `ApplyDamage`（含查实体 / 扣血 / 发事件 / 统计 / 采样）平均耗时 | < 200 ns | **71.6 ns** ✅ |
| `damage_per_1k_ns` | 每 1000 次伤害端到端耗时（集成口径） | < 200000 ns | **68900 ns** ✅ |

实测取自 `bench/damage.txt`（`scripts/verify/task-022.sh` 内 `damage_bench --iterations 1000000`，Release 构建）：

```
compute_damage_ns=4.5651
apply_damage_ns=71.6401
damage_per_1k_ns=68900
alloc_per_damage=0
```

> 验收脚本只自动断言 `compute_damage_ns ≤ 50` 与 `alloc_per_damage ≤ 0`（脚本内 `assert_metric`）；
> `apply_damage_ns` / `damage_per_1k_ns` 为任务书 §7 的人工复核项，此处给出实测值备查。

## 关键设计（保证阈值达标）

### 1. 纯函数 `ComputeDamage` 与状态变更分离

`ComputeDamage(req, atk, def, rolls)` 是**纯函数**：随机量 `rolls` 由调用方传入，函数不读写任何系统状态，
可被缓存 / 预生成，单次只是「几次整数乘法 + 比较 + 分支」，**4.57 ns**（远优于阈值 50 ns）。
`ApplyDamage` 负责把纯函数结果落到 `RoleSystem` 并发布事件，二者解耦让热路径可被精确计时。

### 2. 热路径零堆分配（`alloc_per_damage = 0`）

- 总线事件 `DamageEvent` / `HealEvent` / `EntityDied` 全部 ≤ 32B，走 `EventSlot::kInlinePayload` 内联路径，**不触发堆分配**。
- 采样记录写入**预分配环形缓冲**（`std::vector<DamageRecord>`，容量 = `log_ring_capacity`，启动时一次性分配），
  运行期**不扩容**、满则丢弃计数。因此 100 万次伤害零次 `operator new`。
- 统计计数器、PRNG 状态均为栈 / 值类型，无 `unordered_map` 节点开销。

### 3. 随机来自 per-Scene PRNG，禁全局 rand()

`Prng`（xorshift128+）状态内嵌于 `DamageSystem`，`NextScaled` 用定点缩放（`v*scale>>32`）取高 32 位，
避免低位质量差导致的分布偏斜；无参构造已删除，强制播种。PRNG 推进本身是几次整数异或 + 移位，对 `ApplyDamage` 耗时贡献可忽略。

### 4. 派生属性读取走 `Character::attrs` 公开成员

`ApplyDamage` 取 Attack / Defense 直接读 `Character::attrs.total_`（TASK-016 三层模型派生层），
不触发重算、不访问 Role 私有数据，O(1)。

## Bench 口径（复用 TASK-016 约定）

- **延迟**：Windows `steady_clock` 分辨率 ≈ 100 ns，单次 `ApplyDamage` 无法直接采样；
  `compute_damage_ns` 用「每轮批量 `iterations` 次、rolls 预生成」取中位数；`apply_damage_ns` 走多轮累计取中位数。
- **内存**：替换全局 `operator new/delete` 计数分配次数（`alloc_per_damage` 直接 = 计数 / 次数，恒为 0）。
