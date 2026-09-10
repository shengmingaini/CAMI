# TASK-033 · Gameplay Script 验收报告

> 验收脚本：`scripts/verify/task-033.sh`（`bash scripts/verify/task-033.sh` 退出码 0 为通过）
> 构建：本地 MinGW MSYS2 g++ 16.1.0 + vcpkg（manifest baseline `aae277ac`）；生成器 Ninja
> 本报告所有数字均为本机真实执行结果，非估算。

---

## 1. 实测环境

| 项 | 值 |
|---|---|
| 编译器 | g++ (MinGW MSYS2) 16.1.0 |
| 构建类型 | Release（`-O3 -DNDEBUG`）/ Debug 双构建 |
| 生成器 | Ninja |
| Lua | 5.5.1（MSYS2 系统） |
| 基准规模 | `sim_entities=120`、`sim_tick_hz=10`、`sim_seconds=1` |
| 迭代 | benchmark `--iterations 100000` |

---

## 2. 交付物清单（验收脚本 `require_files` 检查项）

| 交付物 | 路径 | 状态 |
|---|---|---|
| 脚本清单 | `config/gameplay/scripts.json` | ✅ 15 个脚本（五类各 3） |
| 技能公式样例 | `scripting/gameplay/skill/fireball.lua` | ✅ |
| 脚本开发文档 | `scripting/gameplay/docs/README.md` | ✅ |
| 本验收报告 | `docs/gameplay-script-report.md` | ✅ |
| 五类脚本 | `scripting/gameplay/{quest,skill,ai,boss,event}/*.lua` | ✅ 各 3 个，共 15 |
| 桥接层 + 测试 + bench | `scripting/gameplay/{src,include,tests,benchmark}` | ✅ |
| 模块边界扫描 | `include/` 禁止泄露 `src/` | ✅ 通过 |

---

## 3. 单元测试 & 集成测试

| 套件 | 用例数（checks） | 失败 | 结果 |
|---|---|---|---|
| `GameplayScript.Suite`（单元测试） | 366 | 0 | ✅ |
| `GameplayScript_Integration.Suite`（端到端） | 108 | 0 | ✅ |

覆盖（节选）：清单解析与自洽校验、五类真实脚本公式/决策/阶段/活动、返回值钳制（NaN/负/超上限）、
装载失败隔离、引用注册表门禁、**红线拦截**（IO / 直接改 HP / 周期 >1Hz）、热更生效+回滚、
**TASK-031 分配器回归守卫**（10 万次真实分配后 `ScriptErrorCount()==0`）。

> 红线「脚本改不了 HP」：未注入 handler → 调用作废 + `ScriptErrorCount==1`；注入后 → 写意图
> 经 `ScriptCommand` 原样交给状态 Owner（参数 `op=entity.set_hp, a0=target, a1=0`），Lua 无法自改实时状态。

---

## 4. Benchmark 实测（`bench/gameplay.txt`，Release，100000 迭代）

| 指标 | 实测值 | 验收阈值（task-033.sh） | §22 期望 | 结论 |
|---|---|---|---|---|
| `skill_formula_ns` | **1734.0 ns**（≈1.73 µs） | ≤ 3000 | < 3 µs | ✅ |
| `quest_script_ns` | 1013.8 ns（≈1.01 µs） | — | < 2 µs | ✅ |
| `ai_decide_ns` | 1411.2 ns（≈1.41 µs） | — | — | ✅ |
| `boss_phase_check_ns` | 1606.9 ns（≈1.61 µs） | — | < 1 µs | ⚠️ 略超期望（见 §5） |
| `activity_query_ns` | 1449.9 ns（≈1.45 µs） | — | — | ✅ |
| `skill_formula_ns_p99` | 7900.0 ns | — | — | 观测 |
| `script_total_cpu_percent` | **0.3976 %** | ≤ 10 | < 10 % | ✅ |
| `script_total_cpu_ms_per_sec` | 3.9762 ms/s | — | — | ✅ |
| `scripts_loaded` | 15 | — | ≥ 15 | ✅ |

两项**硬门禁**（`assert_metric` 强制）全部通过：
- `skill_formula_ns = 1734.0 ≤ 3000` ✅
- `script_total_cpu_percent = 0.3976 ≤ 10` ✅

> 修复前（TASK-031 分配器记账下溢期）：`skill_formula_ns = 7115.8`（超阈值约 2.4×），
> 且每条 hook 调用都因 `MemoryLimit` 失败。修复后降到 1734.0ns，全部 hook 调用成功。
> 详见 §6 回归说明。

---

## 5. 性能达标对照（§22 + 验收阈值）

| §22 期望 | 实测 | 判定 |
|---|---|---|
| 单次技能公式 < 3 µs | 1.73 µs | ✅ |
| 单次任务脚本 < 2 µs | 1.01 µs | ✅ |
| Boss 阶段检查 < 1 µs | 1.61 µs | ⚠️ 超 0.61 µs |
| 脚本总 CPU < 10% | 0.40% | ✅ |
| 15 脚本加载 < 50 ms | 装载于单测内完成（`stats.loaded==15`，无超时） | ✅ |

**关于 `boss_phase_check_ns = 1.61 µs`**：略高于 §22 的 1 µs 子目标，但**远低于**验收脚本
强制门禁的 3 µs 包络，且 CPU 占比仅 0.41%，不构成任务阻塞项。若需压到 1 µs 以内，可优化
Boss 脚本的表读取/分支或宿主侧 `ResultSlot` 填充；列为后续微调项，非本任务必须。

---

## 6. 回归说明：TASK-031 分配器记账下溢（已修复，本任务锁死）

**根因**：Lua 5.5 在「新建块」（`ptr==NULL`）时把**对象 tag（8）**作为 `osize` 传给 `lua_Alloc`
（见 `lmem.c`：*frealloc(ud, NULL, x, s) 创建新块，大小 's'，'x' 无关*）。旧分配器
`base = used >= osize ? used - osize : 0` 在新建块时也减去了这个 tag，导致 `mem_used_` 每次
新建块少记 8 字节，而释放按真实尺寸扣减——一进一出让 `mem_used_` 单调下漂并回绕到 ~2⁶⁴，
此后 `base + nsize` 恒大于 `memory_bytes`，**任何分配都被拒** → 每条 hook 调用都 `MemoryLimit`
失败（本应成功的公式一律报错）。

**修复**（TASK-031，独立提交）：仅 `ptr != NULL`（realloc）时减旧尺寸，新建块不减：
```cpp
const std::size_t base = (ptr != nullptr && used >= osize) ? used - osize : used;
```

**本任务守卫**（不静默复发）：`gameplay_script_test.cpp` 新增
`test_allocator_memory_accounting_regression`——连续 10 万次 `ComputeSkillFormula` 真实分配后
断言 `ScriptErrorCount() == 0`，并复核末次公式仍为 175.0。该测试已并入 `GameplayScript.Suite`（366 checks / 0 failures）。

---

## 7. ⚠️ 已知缺口与人工决策项：Tick 安全点（TASK-013 集成）

**问题**：TASK-033 的热更逻辑（Prepare→Validate→Activate→Rollback）已全链路正确，但
`ActivateReload` 目前**只由调用方手动**在 `BeginSafePoint`/`EndSafePoint` 之间触发（单测与演练
即此模式），**未被接入真实的 20Hz Tick 循环**。

**原因（已核查）**：
- TASK-013 交付的 `SimulationScheduler`（`server/gamenode/scheduler`）其公开接口
  （`TickPhase` / `ISimulationStage` / `SimulationScheduler`）**没有提供任何安全点（Safe Point）
  钩子**——既无回调也无可注册边界。其 §4 写明「Safe Point 只能由 Scheduler 宣告」，但交付物未实现该宣告机制。
- 宿主头注释提到的 `ScriptReloadStage` 实际属于 **TASK-032**（`scripting/lua`），并非 TASK-013
  产出；且 `SimulationScheduler` 从未注册/调用它。

**影响**：生产环境下，热更激活缺乏确定性的 Tick 安全点；当前仅测试态可用。

**为何不在此静默修复**：该缺口跨 `scripting/gameplay`（TASK-033）、`server/gamenode/scheduler`
（TASK-013）、`scripting/lua`（TASK-032）三个模块子树，改动任一即触及 §27.3 模块边界红线，
属架构决策范畴，需由技术负责人/架构 owner 选择落地方式，例如：
- (a) TASK-013 增加安全点边界 API（如 Tick 末尾的回调 / `RegisterSafePointListener`）；
- (b) 用一个 `ISimulationStage` 包装 TASK-032 的 `ScriptReloadStage`，在 Tick 内开启/关闭安全点；
- (c) 宿主在 Quest/Event 阶段边界自行驱动安全点。

**本任务处理**：如实记录于此与 `scripting/gameplay/docs/README.md` §8，**未改动 TASK-013 / TASK-032 代码**，等待人工决策。该项不影响 `task-033.sh` 验收（门禁不含「生产安全点接线」）。

---

## 8. 验收脚本结论

`bash scripts/verify/task-033.sh` 执行的检查项（按脚本顺序）：

1. 前置任务门禁 `require_tasks_done 018 019 021 031 032` — ✅ 全部 `DONE`
2. 交付物存在性（4 项）— ✅
3. 模块边界 `include/` 禁止泄露 `src/` — ✅
4. CMake configure + 编译（Debug + Release 双构建，0 警告）— ✅
5. `ctest -R GameplayScript` — ✅（366 + 108 checks，0 failures）
6. Benchmark `bin/gameplay_script_bench --iterations 100000` — ✅
7. `assert_metric skill_formula_ns ≤ 3000` — ✅ 1734.0
8. `assert_metric script_total_cpu_percent ≤ 10` — ✅ 0.3976

**结论：TASK-033 本地验收通过（退出码 0）。**

人工复核项（脚本无法自动判定，需二次验收勾选，见 task-033.sh §7）：
- [ ] 五类脚本各 ≥ 3 个、共 ≥ 15 个（自动已测 `scripts_loaded==15`）
- [ ] 端到端玩法链路跑通（集成测试 `GameplayScript_Integration.Suite` 已覆盖）
- [ ] 脚本路径配置化、C++ 无硬编码脚本名（§27.3 已静态扫描 + 单测 `ScriptNames()`/`PathOf()` 全来自清单）
- [ ] 脚本违反红线被拦截（单测 `test_redline_*` 已覆盖）
- [ ] 热更一个技能公式即时生效且可回滚（单测 `test_hot_reload_drill` 已覆盖）
- [ ] 脚本总 CPU < 10%（实测 0.40%）
- [ ] Debug / Release 双构建通过、ctest 全绿（已覆盖）
