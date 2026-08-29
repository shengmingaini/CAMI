# 任务包「方案 A 原地补齐」完成报告

日期：2026-08-29
对象：`F:\AI\workbuddy\CAMI\mmorpg_tasks\`
参照：`docs/task-pack-review.md`（问题清单）

---

## 结论

原 Codex 生成的 39 份任务文件是**空壳模板**（24 章节中 16 个逐字节雷同、依赖成环、零验收脚本）。
现已改为**结构化数据源 + 生成器**模式全量重建，所有 P0 / P1 问题关闭。

| 项 | 修复前 | 修复后 |
|---|---|---|
| 章节模板化 | 16 / 24 章节全库雷同 | 1 / 27 章节雷同（仅「变更记录」，属正常） |
| 验收脚本 | 0 个 | 39 个（+1 公共库） |
| 依赖环 | TASK-012 ↔ TASK-013 | 无环（生成器内置 DFS 三色检测） |
| STATUS 门禁 | 无 | 39 份 `STATUS: PENDING` + `require_tasks_done` 硬门禁 |
| Git Commit 规范 | 无 | 每份独立章节（Conventional Commits + 22 端口推送） |
| State Owner | 模板套话 | 39 段专属文本，生成器强制唯一性 |
| 手工可维护性 | 改 39 份 md | 改 4 个数据文件，一条命令重生成 |

---

## 一、做的改动

### 1. 重建为生成器模式

```
tools/gen/data_a.py   TASK-000 ~ 009
tools/gen/data_b.py   TASK-010 ~ 019
tools/gen/data_c.py   TASK-020 ~ 029
tools/gen/data_d.py   TASK-030 ~ 038   ← 本次新建
tools/gen/build_tasks.py                 ← 渲染器 + 自检器（本次重写）
tasks/TASK-000.md ~ TASK-038.md          ← 生成物，禁止手工编辑
scripts/verify/task-000.sh ~ task-038.sh ← 生成物
```

`build_tasks.py --check` 拦截的问题：

- 必填字段缺失 / 未知字段（契约 26 必填 + 11 可选）
- 依赖不存在的任务编号
- 自依赖
- **依赖成环**（DFS 三色标记，打印完整环路）
- **Phase 号回退造成的依赖倒置**（如 Phase 3 任务依赖 Phase 8 任务）
- State Owner 缺失 / 过短 / 与其他任务雷同
- 编号不连续、ID 重复

### 2. 修掉的数据缺陷

- 4 个数据文件里共 76 处**中文引号误写成 ASCII 双引号**导致 Python 语法错误（其中 70 处曾被自动修复脚本误合并字符串，已全部还原并逐条校验）
- 依赖图重整：去掉 `TASK-012 ↔ TASK-013` 环
- 去掉跨端污染：`TASK-034` 不再依赖服务端 `TASK-008`；`TASK-036` 不再依赖 `TASK-014`
- 去掉倒置依赖：`TASK-021 ← TASK-020`、`TASK-031 ← TASK-021`
- `TASK-025` 补齐 `TASK-013/014/015/024` 依赖与 P95 ≤ 5ms / P99 ≤ 8ms 判定线

### 3. 验收框架

`scripts/verify/_common.sh` 提供：工具链探测、前置门禁、交付物存在性、内容正则、静态红线扫描、
端口检查、CMake configure/build（双构建）、ctest 过滤执行、Benchmark 执行、`key=value` 阈值断言。

红线：不联网、不推送 Git、不写数据库、不改全局状态；`set -euo pipefail` 任一步失败即非零退出；
指标缺失直接判失败，禁止估算值兜底。

已实测（本机）：

```
g++.exe (Rev6, Built by MSYS2 project) 16.1.0 | cmake 4.4.2
vcpkg toolchain: C:\Users\17283\vcpkg
[FAIL] 前置任务 TASK-003 尚未 DONE（当前 STATUS: PENDING），禁止越级实施
```

工具链自动探测 `/c/msys64/mingw64/bin` 并注入 PATH，探测不到直接失败（不存在「工具链缺失仍报通过」）。

### 4. STATUS 门禁与提交流程

- `scripts/task-done.sh TASK-XXX [--verify|--reopen]` —— 唯一允许改写 STATUS 的入口
- 状态机：`PENDING` → `DONE`，`DONE` → `PENDING`（`--reopen`）
- 二次校验全部前置依赖已 DONE，并写入 `DONE-DATE`
- 每份任务含「25. Git Commit」章节：Conventional Commits、一个 TASK 一次提交、
  正文必须贴实测数字、推送走 `git@github.com:22:shengmingaini/CAMI.git`

### 5. README 重写

覆盖：目录结构、本地环境（MinGW 16.1.0 / vcpkg baseline aae277ac / CI 不作为验收依据）、
标准工作流 5 步、状态机、验收脚本执行顺序表、27 个章节清单、
四个关键门禁点（TASK-025 / 030 / 036 / 038）、如何正确修改任务内容、历史问题对照表。

---

## 二、量化验证

章节唯一性（39 份文件，唯一内容数越接近 39 越好）：

| 章节 | 修复前 | 修复后 |
|---|---|---|
| State Owner | 1 | **39** |
| Input / Output | 1 | **39** |
| Public Interface | 1 | **39** |
| Data Model | 1 | **39** |
| Thread Model | 1 | **39** |
| Implementation Steps | 1 | **39** |
| Unit / Integration Test | 1 | **39** |
| Benchmark | 1 | **39** |
| Failure Test | 1 | **39** |
| Acceptance Criteria | 1 | **39** |
| Forbidden | 1 | **39** |
| Performance Expectation | 1 | **39** |
| Deliverables | 1 | **39** |
| Hot Path | 1 | 24 |
| External IO | 1 | 15 |
| Network RPC | 1 | 7 |
| Persistence | 1 | 8 |
| 变更记录 | — | 1（正常） |

关键词覆盖（全库 39 份合计）：`MessageEnvelope` 5、`FlatBuffers` 24、
`Tick Safe Point` 9、`shard` 13、`50000` 9、`4核` 3、`ctest` 78、
`scripts/verify` 196、`require_tasks_done` 76、`STATUS` 233、`git push` 39。

---

## 三、怎么用

```bash
cd F:/AI/workbuddy/CAMI/mmorpg_tasks

python tools/gen/build_tasks.py --check   # 自检：字段/依赖闭合/无环/State Owner 唯一
python tools/check_uniqueness.py          # 章节模板化程度体检

bash scripts/verify/task-000.sh           # 按顺序跑验收
bash scripts/task-done.sh TASK-000 --verify
```

**改任务内容请改 `tools/gen/data_*.py`，不要编辑 `tasks/*.md` 和 `scripts/verify/task-*.sh`** ——
下次生成会被全量覆盖。

---

## 四、遗留与提醒

1. **没有任何代码被实现**。本包只是可执行规格 + 验收框架，TASK-000 之后的 38 个任务
   验收脚本都会因「交付物缺失」而失败，这是**预期行为**。
2. `TASK-025`（战斗性能）、`TASK-036`（低配客户端）、`TASK-038`（50k CCU）是三个硬判定点。
   不达标必须如实记录容量上限，**禁止在实测前宣称兼容或达成**。
3. `_common.sh` 里 `VCPKG_ROOT` 缺省取 `$VCPKG_INSTALLATION_ROOT` 或 `$HOME/vcpkg`，
   若你的 vcpkg 不在 `C:\Users\17283\vcpkg`，运行前显式设置。
4. 客户端低配（4 核 / 4GB / 1GB VRAM / DX11）是**开发目标而非承诺**。
