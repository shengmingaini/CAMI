# TASK-041 · 战斗性能回归与跨进程集成

> 纯验证方（§4 State Owner）：不拥有任何服务状态，只消费 TASK-025（战斗性能矩阵）/
> TASK-030（账本对账）/ TASK-033（Lua 脚本全集）的公开接口做回归，禁止改动被验证实现。

## 1. 范围

本回归分两块：

| 块 | 内容 | 沙箱能否跑 | 入口 |
|---|---|---|---|
| A. 战斗性能回归（沙箱子集） | 进程内真实加载 TASK-033 全部 Lua 脚本 + 重跑 TASK-025 战斗矩阵（含 1000 玩家 / 100% Combat）+ 测量 Lua hook 开销 | **能** | `server/gamenode/combat/benchmark/combat_bench.cpp`（TASK-024 驱动扩展 `--lua-loaded`）→ `bin/combat_bench` |
| B. 账本对账 | TASK-030 `economy_audit.py` 五场景故障下资金守恒 | **能**（纯 Python） | `tools/audit/economy_audit.py` |
| C. 跨进程 E2E | gRPC Bot 走 Login→Gateway→GameNode→Scene→战斗→经济→持久化；TASK-030 五场景故障注入对账 | **不能**（需 Gateway+GameNode+DataService+Redis+MySQL） | 见 §5 |

本仓库交付的是 **A + B** 的沙箱可跑子集，外加 **C** 的脚本骨架与门禁定义。

## 2. 门禁定义（§8 / §22）

回归通过需**同时满足**：

1. `tick_p95_us ≤ 5000`（1000 玩家 / 100% Combat 场景）
2. `tick_p99_us ≤ 8000`
3. `lua_load_ok == 1`（TASK-033 全部 Lua 脚本零装载错误）
4. 退化 < 5%：相对基线（`baseline_tick_p95_us`），基线为 0 时跳过该判定

任一项不满足 → `verdict_pass=0`、`combat_bench` 退出码 1、报告标红（`verdict_reason`）。

报告字段（`bench/combat_regression_lua.txt`，key=value，机器可读）：

```
tick_avg_us=...
tick_p50_us=...
tick_p95_us=...        # 门禁 1
tick_p99_us=...        # 门禁 2
tick_max_us=...
lua_scripts_loaded=... # TASK-033 实际装载脚本数
lua_load_ok=1          # 门禁 3
lua_load_ms=...        # Lua 全集加载耗时
lua_skill_formula_ns=...  # TASK-033 单次「技能公式」hook 平均开销（ns）
lua_cpu_percent=...    # TASK-033 脚本层 CPU 占世界时间的百分比
baseline_tick_p95_us=0 # 沙箱未持久化基线，跳过退化判定
baseline_tick_p99_us=0
verdict_pass=1
verdict_reason=ok
```

## 3. 阈值来源

- `tick_p95 ≤ 5000 / tick_p99 ≤ 8000`：TASK-025 架构可行性判定点（§8 矩阵必须 4 规模 × 5 场景全跑，禁止抽样）。
- `退化 < 5%`：TASK-033 Lua 在 Combat Tick 内落地后，禁止其拖垮 Tick（§22 Lua 单次技能公式 < 3µs 为单点指标，本回归看端到端 p95/p99）。
- `lua 零装载错误`：TASK-033 五类脚本各 ≥ 3 个，装载期 fail-fast（`require_all_scripts=true`）。

## 4. 运行方式

```bash
# 双构建（Debug + Release，0 警告）
bash scripts/verify/task-041.sh        # 走完整验收：编译 + ctest + bench + 阈值断言
# 或手动：
BUILD_TYPE=Release cmake_build_both    # 见 scripts/verify/_common.sh
BUILD_TYPE=Debug   cmake_build_both

# ctest（沙箱子集）
ctest -R QA_E2E                      # 从仓库根跑，WORKING_DIRECTORY=repo root

# 战斗性能回归（落盘 bench/combat_regression_lua.{txt,json}）
bin/combat_bench --matrix --lua-loaded --duration 60 --warmup 5 --out bench/combat_regression_lua.json

# 账本对账（TASK-030，纯 Python）
python3 tools/audit/economy_audit.py

# 一条龙（推荐）
bash tools/qa/regression/run.sh
```

参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--matrix` | off | 是否跑完整 4×5 矩阵；verify 脚本与 run.sh 均带此旗。不带时仅跑 1000/100pct 单场景 |
| `--lua-loaded` | — | 回归模式开关：进程内真实加载 TASK-033 Lua 全集 + 重跑战斗矩阵 + Lua 探针并落盘报告 |
| `--duration` | 60 | 单场景计时时长（秒，20Hz 真实节拍，共 duration×20 个 Tick） |
| `--warmup` | 5 | 单场景预热时长（秒，数据丢弃） |
| `--out` | bench/combat_regression_lua.json | 矩阵输出 JSON 路径（报告 txt 永远写 bench/combat_regression_lua.txt） |

> ctest（`qa_e2e_test`）走 `E2EScenario` 单测路径：`matrix_full=false` + `duration=3` + `warmup=1`，仅 1000/100pct 单场景加速，不替代发布前的 `--matrix` 全量。

> `real_infra` 路径（`E2EConfig::real_infra=true`）在 `E2EScenario::Run` 内显式失败（需真实 gRPC+Redis+MySQL，见 §5），不静默通过。

## 5. 真实跨进程部分（沙箱不执行）

以下需在「Gateway + GameNode + DataService（Redis+MySQL）全在线」的完整环境执行，本沙箱无外部服务，故 `E2EConfig::real_infra=true` 路径显式返回 `INTERNAL_ERROR` 并说明所需依赖，不静默通过：

- **C.1 跨进程 E2E**：真实 Bot 走 Login→Gateway→GameNode→EnterScene→Attack→Loot→Trade→Persist，断言每阶段可观测（Session 七字段、Scene 创建、战斗伤害、掉落入库、交易对账）。
- **C.2 账本对账（故障注入）**：在真实链路上复跑 TASK-030 五场景（进程崩溃 / 网络分区 / 重复提交 / 幂等键冲突 / 部分写入），断言资金守恒，不重复扣/发/复制。
- **C.3 退化定位**：退化 > 5% 时，报告标红并定位耗时 Lua 脚本（来自 `lua_load_errors` / hook 探针），不静默通过。

真实环境的验收由 `scripts/verify/task-041.sh` 的人工复核清单（步骤 7）覆盖，且需在具备基础设施的机器上手动跑 `E2EScenario` 的 `real_infra=true` 分支与 `economy_audit` 的故障注入模式。

## 6. 频率建议

- 每次 **Lua / 战斗改动后** 必跑（`bash tools/qa/regression/run.sh`），作为常驻门禁。
- 每次 **发布前** 跑完整 `--matrix`（4×5 全量，禁止抽样）。
- 退化接近 5% 时提前告警，定位瓶颈 Lua 脚本。

## 7. 禁止项（§21 Forbidden）

- 禁止在回归中改动被验证模块（TASK-025/030/033）的实现。
- 禁止抽样跑矩阵（必须 5 场景 × 4 规模；`--no-matrix` 仅用于 ctest 加速，不替代发布前全量）。
- 禁止在退化 > 5% 时静默通过。
- 禁止 E2E 残留僵尸进程（真实环境必须清理）。
- 禁止用估算值代替实测（报告只写真实 `key=value`）。
