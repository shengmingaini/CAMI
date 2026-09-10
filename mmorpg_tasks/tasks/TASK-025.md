---
TASK-ID: TASK-025
NAME: Combat Benchmark（架构可行性判定点）
PHASE: Phase 5 · 战斗
MODULE: benchmark/combat
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DONE-DATE: 2026-09-08
DEPENDENCIES: TASK-013, TASK-014, TASK-015, TASK-024
---

# TASK-025 · Combat Benchmark（架构可行性判定点）

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-025` |
| NAME | Combat Benchmark（架构可行性判定点） |
| PHASE | Phase 5 · 战斗 |
| MODULE | `benchmark/combat` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-013`, `TASK-014`, `TASK-015`, `TASK-024` |

---

## 1. Objective

独立性能模块：在 100 / 300 / 500 / 1000 玩家规模、Idle / Movement / 10% Combat / 50% Combat / 100% Combat 五种场景下测量 Tick 与分阶段耗时。**这是第一次真正判定当前架构能否向 50,000 CCU 方向继续走。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-013` · Simulation Scheduler（20Hz 固定 Tick）
- `TASK-014` · AOI System（Dynamic Grid 第一版）
- `TASK-015` · Movement System
- `TASK-024` · Combat Framework

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 013 014 015 024`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`benchmark/combat`

## 4. State Owner（状态归属）

Benchmark 是纯观测者，不拥有任何服务状态，被测 Scene 的 Owner 不变（仍是 Scene）。Benchmark 只允许读与统计，禁止在测量过程中改动仿真逻辑或调整参数。所有数字必须原样落盘，禁止后处理、四舍五入美化或删除不达标的组。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-013 分阶段计时；TASK-014/015/024 各系统；TASK-011~024 全部 GameNode 核心

## 6. Output

benchmark 套件 + 五场景 × 四规模矩阵报告 + 架构可行性结论文档

## 7. Public Interface

```cpp
namespace mmo::bench {
struct ScenarioConfig { uint32_t player_count; float combat_ratio;   // 0.0 ~ 1.0
                        bool movement_enabled; uint32_t duration_seconds{60};
                        uint32_t warmup_seconds{5}; uint32_t seed{42}; };
struct ScenarioResult {                       // 分阶段，禁止只报总 Tick
  uint32_t player_count; float combat_ratio;
  uint64_t tick_avg_us, tick_p50_us, tick_p95_us, tick_p99_us, tick_max_us;
  uint64_t phase_us[8];                       // Input..Replication 各阶段 P95
  uint64_t aoi_avg_visible; uint64_t combat_events_per_sec;
  size_t   peak_rss_mb; double cpu_percent; uint64_t msgs_out_per_sec; };
class CombatBenchmark { public:
  core::Result<ScenarioResult> Run(const ScenarioConfig&);
  core::Result<void> RunMatrix(std::string_view output_dir);   // 5 场景 × 4 规模
  core::Result<void> ExportJson(const ScenarioResult&, std::string_view path); };
}
```

## 8. Data Model

**测试矩阵（必须全跑，禁止抽样）**

| 玩家数 | Idle | Movement | 10% Combat | 50% Combat | 100% Combat |
|---|---|---|---|---|---|
| 100 | ✓ | ✓ | ✓ | ✓ | ✓ |
| 300 | ✓ | ✓ | ✓ | ✓ | ✓ |
| 500 | ✓ | ✓ | ✓ | ✓ | ✓ |
| 1000 | ✓ | ✓ | ✓ | ✓ | ✓ |

**通过线（1000 玩家单 Scene）**

| 指标 | 目标 |
|---|---|
| Average Tick | < 5ms |
| P95 Tick | ≤ 5ms |
| P99 Tick | ≤ 8ms |

**未达标处理**：不进入 TASK-026，先做性能分析与架构复盘（写 docs/perf-analysis.md），必要时提 RFC。

## 9. Thread Model

Benchmark 进程独立于服务进程；可指定绑核（taskset / SetThreadAffinityMask）减少噪声。禁止在 benchmark 中做无关 IO。

## 10. Hot Path

**YES** （测量对象即热路径）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**YES** （写报告文件）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- benchmark/combat/
- benchmark/common/
- docs/benchmark/
- tools/report/

## 15. Implementation Steps

1. 实现 benchmark/common/metrics.h：分位数统计（HDR histogram 或固定桶，禁止全量排序）
2. 实现 benchmark/common/scenario.h：ScenarioConfig 与场景构造（批量生成玩家/怪物实体）
3. 实现 benchmark/common/system_probe.h：CPU / RSS / 线程数采样（Windows 用 PDH / GetProcessMemoryInfo）
4. 实现 combat_benchmark.h/.cpp：Run / RunMatrix / ExportJson，复用 TASK-013 的分阶段计时
5. 实现五场景：Idle（仅心跳）、Movement（全员随机移动）、10%/50%/100% Combat（按比例实体进入战斗并循环放技能）
6. 实现确定性：固定 seed，同配置可复现（结果差异 < 5%）
7. 实现 warmup：前 5 秒数据丢弃（避免 JIT/预热噪声）
8. 实现报告导出：JSON + Markdown 表格，含分阶段 P95
9. 跑完整矩阵（5 × 4 = 20 组，每组 60 秒 + 5 秒预热）
10. 写 docs/benchmark/combat-report.md：全量数据 + 分阶段瓶颈分析 + 是否达标结论
11. 若不达标：写 docs/perf-analysis.md（火焰图/采样数据 + 瓶颈定位 + 三种改进方案与代价评估），**不进入下一任务**

## 16. Unit Test

分位数统计正确性（注入已知分布验证 P95/P99）；场景构造实体数正确；确定性（同 seed 两次结果差异 < 5%）；报告导出字段完整

## 17. Integration Test

完整矩阵 20 组全部跑通并导出报告；报告可被 tools/report/compare.py 解析并生成趋势对比；与手动 1000 玩家单场景跑的结果差异 < 10%

## 18. Benchmark

bin/combat_bench（本任务自身即 benchmark）：输出 20 组 `tick_p95_us` / `tick_p99_us` / 各阶段 `phase_p95_us` / `peak_rss_mb` / `cpu_percent`

## 19. Failure Test

某个规模跑挂（OOM/超时）：记录失败点并降级到更小规模继续，报告中明确标注「未覆盖」；机器负载波动导致结果异常：自动重跑该组 3 次取中位，并在报告标注标准差；预热不足导致首组偏高：warmup 机制验证（对比有无 warmup 的差异）

## 20. Acceptance Criteria

1. **5 场景 × 4 规模 = 20 组全部跑完**（禁止抽样，报告必须含完整矩阵）
2. 每组输出 Average / P50 / P95 / P99 / Max Tick 与八阶段 P95 分解
3. 1000 玩家场景：Average < 5ms、P95 ≤ 5ms、P99 ≤ 8ms（最差场景即 100% Combat 也要达标；若 Idle 达标而 Combat 不达标，判定**不通过**）
4. 报告含 CPU / 内存 / 消息量数据
5. 同配置可复现（两次结果差异 < 5%）
6. docs/benchmark/combat-report.md 存在且含明确「通过/不通过」结论
7. 若不通过，docs/perf-analysis.md 存在且含瓶颈定位与改进方案（并暂停后续任务）

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止只报告总 Tick 时间（必须分阶段）
- 禁止抽样跑（必须 20 组全跑）
- 禁止在达标前进入 TASK-026（这是硬性门禁）
- 禁止在无 warmup 的情况下采信数据
- 禁止删除或美化不达标的数字
- 禁止用更高配置机器掩盖不达标（必须记录机器规格）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

1000 玩家 100% Combat：Average Tick < 5ms、P95 ≤ 5ms、P99 ≤ 8ms；单玩家服务端内存 < 64KB；1000 玩家下行带宽 < 2Mbps/玩家。全部数字以实测为准，写入 docs/benchmark/combat-report.md。

## 23. Deliverables

- benchmark/combat/combat_benchmark.h
- benchmark/combat/src/*.cpp
- benchmark/common/*.h
- benchmark/common/src/*.cpp
- tools/report/compare.py
- docs/benchmark/combat-report.md
- docs/benchmark/matrix.json
- docs/perf-analysis.md（不达标时）

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-025.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-025.sh
bash scripts/verify/task-025.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 013 014 015 024`
2. 交付物存在性检查（4 项）
3. CMake configure + 编译（Release 单构建）
4. ctest 过滤执行：`-R Bench_Combat`
5. Benchmark 执行：`bin/combat_bench --matrix --duration 60 --warmup 5 --out bench/combat_matrix.json`
6. 性能阈值断言：`bench/combat_1000_100pct.txt` 中 `tick_p95_us` ≤ `5000`
7. 性能阈值断言：`bench/combat_1000_100pct.txt` 中 `tick_p99_us` ≤ `8000`
8. 性能阈值断言：`bench/combat_1000_100pct.txt` 中 `tick_avg_us` < `5000`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-025

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(bench): Combat Benchmark（架构可行性判定点）

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-025.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-025
EOF

# 3) 推送：GFW 屏蔽 ssh.github.com，必须走 22 端口
git push git@github.com:22:shengmingaini/CAMI.git main
```

提交规范：

- 类型：`feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build` / `chore`（本任务建议 `feat`）
- **一个 TASK 一次独立提交**，禁止把多个 TASK 合并提交
- 正文必须包含实测数字，禁止「性能良好 / 已优化」这类无法验证的描述
- 未通过验收脚本禁止提交，禁止 `--no-verify` 绕过

## 26. Codex Execution Rules

1. 读规范：先读 `PROJECT_REQUIREMENTS.md` 与本任务涉及章节，架构冻结，不得自行推翻。
2. 读任务：完整读完本文件全部章节再动手，禁止只看 Objective 就开始写。
3. 查依赖：确认 TASK-013, TASK-014, TASK-015, TASK-024 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-025.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-013` · `server/gamenode/scheduler`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-014` · `server/gamenode/aoi`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-015` · `server/gamenode/movement`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-024` · `server/gamenode/combat`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

### 27.3 模块边界红线（全任务统一）

- 模块 ≠ 进程：本任务代码只落在自身 `module` 子树（`include/` + `src/` + `tests/` + `docs/`），禁止扩散到其它任务拥有的目录。
- 下游只能通过本任务 `include/` 下的**公开头与接口**调用，禁止 `#include` 本任务 `src/` 或内部头（验收脚本会静态扫描本任务 `include/` 是否泄露内部 `src/`）。
- 本任务只调用依赖模块**声明**的接口，禁止访问其内部数据（如 `otherModule.internalData` 模式）。
- 接口在 `STATUS: DONE` 之后变更必须走 `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。
- 依赖方向单向（Game → Gameplay → Core），禁止循环依赖；新增模块不得破坏既有依赖环约束。

### 27.4 扩展性约束（可扩展框架兼容性）

- 新增同类能力（新 Command / 新 Event / 新 Scene 类型 / 新模块）必须走**注册表 / ID 段**机制，禁止在 `switch` 里硬编码穷举。
- 跨模块扩展点统一用抽象（C++ Interface / Command / Event），新增实现**不得修改既有任务文件**。
- 协议 / 接口变更必须带 `version` 字段并向下兼容，旧客户端 / 旧模块不得因此断连或编译失败。
- 所有模块遵循统一目录模板（include/src/tests/benchmark/docs/CMakeLists.txt）与五文档契约（README/INTERFACE/DEPENDENCY/PERFORMANCE/TEST），新增模块不得例外。

> 模块归属表（谁拥有哪棵子树）：本任务的 `module` 字段即其独占目录；
> 其它任务的 `module` 字段不得被本任务写入。统一模块模板见 `DEVELOPMENT.md` / 根规范 §6。

## 28. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-08-29 | 方案 A 原地补齐：由 `tools/gen/build_tasks.py` 从结构化数据源重新生成，补齐 State Owner / 验收脚本 / STATUS 门禁 / Git Commit 规范 |
| 2026-08-29 | 完善：新增 §27 接口契约/模块边界/扩展性（全任务统一，防相互干扰）；新增 TASK-039 Social / TASK-040 ControlService / TASK-041 集成与回归；依赖相位自检跳过最终交付汇点；Scene Migration 登记为 Phase 2 RFC |
