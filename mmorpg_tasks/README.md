# MMORPG Task Pack（TASK-000 ～ TASK-041）

按《Project Requirements V1.0 — Frozen Architecture》冻结架构逐步实施的任务包。
42 份任务规格 + 42 个本地验收脚本，**全部由 `tools/gen/` 从结构化数据源生成**。

---

## 0. 三条不可协商的红线

1. **验收只在本地**：CI（GitHub Actions）不作为验收依据。所有编译、测试、Benchmark 必须在本地
   MinGW MSYS2 + vcpkg 环境跑通。
2. **越级即失败**：任一前置任务 `STATUS` 不是 `DONE`，验收脚本立即非零退出，禁止跳过。
3. **禁止伪造结果**：验收脚本只报告真实执行结果。指标缺失直接判失败，禁止估算值、禁止美化数字。

---

## 1. 目录结构

```
mmorpg_tasks/
├── README.md                     ← 本文件
├── tasks/
│   ├── TASK-000.md … TASK-041.md ← 任务规格（生成物，禁止手工编辑）
│   └── …                          （每份含 YAML 头：STATUS / DEPENDENCIES / PHASE）
├── scripts/
│   ├── verify/
│   │   ├── _common.sh            ← 验收公共库（门禁/编译/ctest/bench/阈值/红线扫描）
│   │   └── task-000.sh … task-041.sh  ← 每个任务一个验收脚本（生成物）
│   └── task-done.sh              ← 唯一允许改写 STATUS 的入口
├── tools/
│   ├── gen/
│   │   ├── data_a.py             ← TASK-000 ~ 009 数据源
│   │   ├── data_b.py             ← TASK-010 ~ 019
│   │   ├── data_c.py             ← TASK-020 ~ 029
│   │   ├── data_d.py             ← TASK-030 ~ 041
│   │   └── build_tasks.py        ← 渲染器 + 自检器
│   └── check_uniqueness.py       ← 章节模板化程度自检
└── docs/                         ← 规划与评审文档
```

---

## 2. 本地环境（唯一可信验证路径）

| 项 | 值 |
|---|---|
| 编译器 | MinGW MSYS2 g++ **16.1.0**（`C:\msys64\mingw64\bin`） |
| CMake | 4.4.2+ |
| 依赖管理 | vcpkg manifest mode，baseline `aae277ac` |
| Shell | MSYS2 MinGW64 shell（Git Bash 亦可，脚本会自动注入 MinGW PATH） |
| CI | **不作为验收依据**（账户计费不可靠），仅作参考 |

脚本会自动探测 `/c/msys64/mingw64/bin` 并注入 PATH；探测不到 g++ / cmake / ctest 直接失败退出，
不存在「工具链缺失但仍报告通过」的情况。

`VCPKG_ROOT` 缺省取 `$VCPKG_INSTALLATION_ROOT` 或 `$HOME/vcpkg`，可用环境变量覆盖。

---

## 3. 标准工作流（每个 TASK 都一样）

```bash
# ① 生成 / 自检任务包（改了数据或想校验完整性时）
python tools/gen/build_tasks.py --check      # 只自检：字段、依赖闭合、无环、State Owner 唯一

# ② 实施：读 tasks/TASK-XXX.md，按 Implementation Steps 落地

# ③ 跑验收（Release；Debug 用 BUILD_TYPE=Debug）
bash scripts/verify/task-XXX.sh

# ④ 通过后才允许标记 DONE（--verify 表示先跑验收再改状态）
bash scripts/task-done.sh TASK-XXX --verify

# ⑤ 提交（Conventional Commits，一个 TASK 一次提交）
git add -A
git commit -F - <<'EOF'
feat(scheduler): 实现 20Hz 固定 Tick 与八阶段调度

- 实测数字：（粘贴验收脚本真实输出）
Refs: TASK-013
EOF
git push git@github.com:22:shengmingaini/CAMI.git main   # GFW 屏蔽 ssh.github.com，走 22 端口
```

打回时：`bash scripts/task-done.sh TASK-XXX --reopen`

---

## 4. STATUS 状态机

```
PENDING ──(scripts/task-done.sh)──> DONE
   ↑                                  │
   └────(scripts/task-done.sh --reopen)┘
```

- 只允许 `PENDING` / `BLOCKED` → `DONE`；只有 `DONE` 能 `--reopen` 回 `PENDING`。
- `task-done.sh` 会二次校验全部前置依赖已 `DONE`（双保险，验收脚本里也查一次）。
- 严禁手工 `sed` 改 STATUS —— 用脚本，脚本会留 `DONE-DATE`。

---

## 5. 验收脚本做了什么

按顺序执行，任一步失败即非零退出（`set -euo pipefail`）：

| 步骤 | 函数 | 说明 |
|---|---|---|
| 工具链探测 | `require_toolchain` | 缺 g++/cmake/ctest 直接失败 |
| 前置门禁 | `require_tasks_done` | 前置任务必须 `STATUS: DONE` |
| 交付物存在 | `require_files` | 跳过含 `*` / 中文说明的条目 |
| 交付物内容 | `require_content` | 文件内必须匹配指定正则 |
| 静态红线扫描 | `scan_forbidden` | 如热路径禁 `mysql\|redis\|grpc`、禁 `std::thread`、禁裸 `throw` |
| 端口检查 | `require_free_port` | Redis 6379 / MySQL 3306 等 |
| 编译 | `cmake_build_both` / `cmake_build` | 双构建或单构建，**CI 不算** |
| 单测 | `run_ctest` | `ctest --output-on-failure -R <Pattern>` |
| Benchmark | `run_bench` | 真实执行二进制 |
| 阈值断言 | `assert_metric` | 从 `key=value` 输出提取，`le` / `lt` / `ge` |
| 人工复核 | `info` | 列出 §20 中脚本无法自动判定的条目，需人工勾选 |

脚本**不联网、不推送 Git、不写数据库、不改全局状态**。

---

## 6. 任务规格的 28 个章节

1. Objective　2. Dependencies　3. Module　**4. State Owner**　5. Input　6. Output
7. Public Interface　8. Data Model　9. Thread Model　10. Hot Path　11. External IO
12. Network RPC　13. Persistence　14. Files　15. Implementation Steps
16. Unit Test　17. Integration Test　18. Benchmark　19. Failure Test
20. Acceptance Criteria　21. Forbidden　22. Performance Expectation
23. Deliverables　**24. Verification Script**　**25. Git Commit**　26. Codex Execution Rules
**27. 接口契约、模块边界与扩展性**　28. 变更记录

其中 **State Owner 为 42 段专属文本**（由生成器强制唯一性校验），明确回答「这个状态谁有权写」。
**§27 是本轮完善新增的统一契约**——所有任务共用同一套「模块边界红线 + 扩展性约束 + 消费上游接口清单」，专门防任务间交付相互干扰、保框架可扩展（详见 §10）。

---

## 7. 关键门禁点

| TASK | 判定内容 | 不达标的后果 |
|---|---|---|
| **TASK-025** | 1000 玩家 Tick：Avg < 5ms、P95 ≤ 5ms、P99 ≤ 8ms（5 场景 × 4 规模 = 20 组全跑） | **架构可行性判定点**，禁止进入 TASK-026 |
| TASK-030 | 五场景故障下不重复扣钱 / 发奖 / 复制装备 | 禁止进入 Lua 阶段 |
| TASK-036 | Low 档 4 核 / 4GB / 1GB VRAM 实测 | 如实记录容量上限，禁止宣称兼容 |
| TASK-038 | 100 → 50000 CCU 七级阶梯 | 不达标必须如实写容量上限，禁止估算替代 |

> 客户端低配（4 核 / 4GB / 1GB VRAM / DX11）是**开发目标而非承诺**，最终以 TASK-036 / 038 实测为准。

---

## 8. 如何修改任务内容

**不要直接编辑 `tasks/*.md` 或 `scripts/verify/task-*.sh`** —— 下次生成会被全量覆盖。

正确做法：

1. 改 `tools/gen/data_*.py` 中对应 TASK 的字段；
2. 若新增字段，同步更新 `build_tasks.py` 里的 `REQUIRED` 或 `OPTIONAL`；
3. `python tools/gen/build_tasks.py --check` 自检；
4. `python tools/gen/build_tasks.py` 重新生成；
5. `python tools/check_uniqueness.py` 确认没有章节退化成模板套话。

生成器自检会拦截：字段缺失、未知字段、依赖不存在、自依赖、**依赖成环**、
**Phase 号回退造成的依赖倒置**、State Owner 缺失或雷同、编号不连续。

---

## 9. 本包的历史问题（已修复）

| 问题 | 修复方式 |
|---|---|
| 24 个章节中 16 个在 39 份文件里逐字节相同 | 改为结构化数据源 + 渲染器，27 章节中 26 个有真实差异 |
| 依赖成环（TASK-012 ↔ TASK-013） | 重整依赖图，生成器内置 DFS 三色环检测 |
| 零验收脚本 | 39 个脚本，覆盖门禁/编译/ctest/bench/阈值/红线扫描 |
| Acceptance Criteria 为通用八条 | 每份专属，并注入验收脚本作为人工复核清单 |
| 无 STATUS 门禁 | YAML 头 `STATUS: PENDING` + `require_tasks_done` 硬门禁 |
| 无 commit 规范 | 每份含「Git Commit」章节，Conventional Commits + 22 端口推送 |
| 目录命名违反根规范 | 统一 `engine/ server/ client/ protocol/ database/ scripting/ benchmark/ tools/` |

评审报告见 `F:\AI\workbuddy\CAMI\docs\task-pack-review.md`。

---

## 10. 本轮完善：防相互干扰 / 可扩展 / 缺口补齐（2026-08-29）

在「方案 A 原地补齐」的基础上，针对用户要求「功能模块化、统一接口、防任务间交付相互干扰、可扩展框架兼容性、补充缺失内容」做了以下强化。

### 10.1 防任务间交付相互干扰 —— §27 统一契约

每份任务规格新增 **§27 接口契约、模块边界与扩展性**，由生成器强制渲染，含四段：

- **§27.1 导出接口**：本任务 `include/` 下的公开头/接口即契约，一旦 `STATUS: DONE` 视为冻结，下游依赖之。
- **§27.2 消费上游接口清单**：从依赖自动推导「本任务消费哪些前置任务的公开接口」，并标注「禁止 `#include` 其 `src/`」。
- **§27.3 模块边界红线**（全任务统一）：
  - 代码只落在自身 `module` 子树，禁止扩散到他人目录；
  - 下游只能调本任务 `include/` 公开头，禁止 `#include` 本任务 `src/` 或内部头；
  - **验收脚本静态扫描**：`scan_forbidden '<module>/include' '#include\s+["<][^">]*src/[^">]*'` 强制公开头不得泄露内部 `src/`；
  - 接口 DONE 后变更必须走 `version` + 兼容性评估，禁止静默改签名；
  - 单向依赖、禁止循环。
- **§27.4 扩展性约束**：新增同类能力走注册表 / ID 段（禁 `switch` 硬编码穷举）；跨模块扩展点用抽象（Interface / Command / Event）；协议带 `version` 向下兼容；统一目录模板 + 五文档契约。

### 10.2 补齐四个功能缺口

可行性分析（`docs/feasibility-analysis.md`）指出原 39 任务缺四类能力，本轮补齐：

| 新增任务 | 补齐内容 | 依赖 |
|---|---|---|
| **TASK-039** Social System（组队/好友/公会/聊天/邮件） | 第一版单 GameNode 内全量内存态；世界频道走 Gateway 订阅表，**禁全服 O(N) 广播**；公会/邮件持久化经 DataService | TASK-007 / 011 / 016 / 028 |
| **TASK-040** ControlService（控制面） | 补齐「四进程」中唯一缺失实现：节点注册表权威同步、Config Version 下发、健康检查、Gateway 多实例注册；崩溃降级不雪崩 | TASK-003 / 006 / 010 |
| **TASK-041** 跨进程集成与战斗回归 | 补齐跨进程全链路验证 + Lua 落地后的战斗性能回归（退化 > 5% 判失败） | TASK-025 / 030 / 033 |

任务总数 **39 → 42**。

### 10.3 消解架构张力 —— Live Scene Migration 登记为 RFC

根规范 §1（最终目标含 Live Scene Migration）与 §34（第一版只做 Reconnect/Reattach）存在张力。本轮不强行塞进第一版，而是登记为 **Phase 2 RFC**：`docs/rfc/scene-live-migration.md`，明确背景、第一版现状、Phase 2 待解问题、接口兼容约束与验收门禁。TASK-037 验收文案同步引用该 RFC。

### 10.4 自检增强

- 依赖相位「倒置」检测改为**跳过最终交付汇点**（无任何任务依赖它的任务可合法依赖所有阶段），避免 TASK-038 被误报。
- 全量重新生成后 `--check` 通过：42 任务、字段完整、依赖闭合、无环、无自依赖、State Owner 全唯一。
- `check_uniqueness.py`：28 章节中仅「变更记录」雷同（预期内），无章节退化成模板套话。

评审报告见 `F:\AI\workbuddy\CAMI\docs\task-pack-review.md`；可行性分析见 `F:\AI\workbuddy\CAMI\docs\feasibility-analysis.md`。
