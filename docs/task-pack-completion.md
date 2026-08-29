# MMORPG Task Pack · 完善完成报告（2026-08-29）

> 围绕用户指令「功能模块化、统一接口、防任务间交付相互干扰、可扩展框架兼容性、补充缺失内容」对任务包做的第二轮强化。
> 第一轮（方案 A 原地补齐）见 `docs/task-pack-fix-report.md`；可行性分析见 `docs/feasibility-analysis.md`。

---

## 1. 改动总览

| 项 | 改动前 | 改动后 |
|---|---|---|
| 任务总数 | 39（TASK-000 ~ 038） | **42**（TASK-000 ~ 041） |
| 任务规格章节 | 27 章 | **28 章**（新增 §27 统一契约，原变更记录顺延 §28） |
| 验收脚本 | 39 个 | **42 个**（每个含模块边界静态扫描） |
| 缺失进程实现 | ControlService 无独立任务 | TASK-040 补齐 |
| 社交系统 | 仅在规范文字描述 | TASK-039 落地规格 |
| 跨进程 / 回归验证 | 缺口 | TASK-041 补齐 |
| Live Scene Migration | §1 与 §34 张力未消解 | 登记为 Phase 2 RFC |

---

## 2. 防任务间相互干扰：§27 统一契约

每个任务规格由生成器强制渲染 **§27 接口契约、模块边界与扩展性**，分四段：

- **§27.1 导出接口（冻结契约）**：本任务 `include/` 下的公开头/接口即契约，一旦 `STATUS: DONE` 视为冻结，下游依赖之。
- **§27.2 消费上游接口清单**：从 `DEPENDENCIES` 自动推导「本任务消费哪些前置任务的公开接口」，并标注「禁止 `#include` 其 `src/`」——防止绕过契约直接啃内部实现。
- **§27.3 模块边界红线（全任务统一）**：
  - 代码只落在自身 `module` 子树，禁止扩散到他人目录；
  - 下游只能调本任务 `include/` 公开头，禁止 `#include` 本任务 `src/` 或内部头；
  - **机器门禁**：验收脚本静态扫描 `scan_forbidden '<module>/include' '#include\s+["<][^">]*src/[^">]*'`，公开头一旦泄露内部 `src/` 直接 fail；
  - 接口 DONE 后变更必须走 `version` + 兼容性评估，禁止静默改签名导致下游编译失败；
  - 单向依赖、禁止循环。
- **§27.4 扩展性约束**：新增同类能力走注册表 / ID 段（禁 `switch` 硬编码穷举）；跨模块扩展点用抽象（Interface / Command / Event）；协议带 `version` 向下兼容；统一目录模板 + 五文档契约。

**效果**：任一任务交付时，下游只能依赖其 `include/` 契约，且契约变更受版本管控——从机制上杜绝「A 改了内部实现、B 半夜编译挂掉」这类相互干扰。

---

## 3. 三个新增任务

| TASK | 模块 | 补齐的架构缺口 | 关键约束 |
|---|---|---|---|
| **TASK-039** Social System | `server/gamenode/social` | 组队/好友/公会/聊天/邮件落地规格（原仅在规范文字） | 单 GameNode 内存态；世界频道走 Gateway 订阅表，**禁全服 O(N) 广播**；公会/邮件持久化经 DataService；社交写操作全走 Command/Event |
| **TASK-040** ControlService | `server/control` | 「四进程」中唯一缺失的运行实现 | 节点注册表权威同步、Config Version 下发、健康检查、Gateway 多实例注册；不拥有任何游戏状态；崩溃降级不雪崩 |
| **TASK-041** 跨进程集成与战斗回归 | `tools/qa` | 跨进程全链路 + Lua 落地后战斗回归缺口 | 真实 gRPC+Redis+MySQL E2E；重跑 TASK-025 的 1k 战斗矩阵；Lua 后 tick_p95≤5000 / p99≤8000，**退化 > 5% 判失败** |

依赖关系已并入生成器自检：TASK-039→{007,011,016,028}，TASK-040→{003,006,010}，TASK-041→{025,030,033}，TASK-038 依赖补入上述三项。

---

## 4. 架构张力消解：Live Scene Migration RFC

根规范 §1（最终目标含 Live Scene Migration）与 §34（第一版只做 Reconnect/Reattach/Session Recovery）存在张力。本轮不强行塞进第一版，登记为 **Phase 2 RFC**：

- 文件：`mmorpg_tasks/docs/rfc/scene-live-migration.md`
- 内容：背景、第一版现状、Phase 2 待解问题、接口兼容约束、验收门禁。
- TASK-037 验收文案同步引用该 RFC，明确「按 §34 执行、Live Migration 留 Phase 2」。

---

## 5. 自检与验证结果

| 验证项 | 命令 / 位置 | 结果 |
|---|---|---|
| 字段/依赖/环/自依赖/State Owner | `python tools/gen/build_tasks.py --check` | ✅ 42 任务、字段完整、依赖闭合、无环、无自依赖、State Owner 全唯一 |
| 章节未退化 | `python tools/check_uniqueness.py` | ✅ 28 章仅「变更记录」雷同（预期），其余有信息量 |
| 模块边界静态扫描 | `scripts/verify/task-039/040/041.sh` 第 3 步 | ✅ 均含 `scan_forbidden ... src/` |
| TASK-041 §7 渲染 bug | `data_d.py` 行 1039 多余 `"` | ✅ 已修，重生成后 ```` ```cpp ```` 干净 |
| 生成产物计数 | `ls tasks/`（42）、`ls scripts/verify/`（43，含 `_common.sh`） | ✅ 一致 |

> 注：本次仅修改生成器数据源与渲染器，**未触碰任何已实施的业务代码**；任务规格全部由 `build_tasks.py` 重新生成。

---

## 6. 使用与下一步

1. **改任务内容**：永远改 `tools/gen/data_*.py`，再 `python tools/gen/build_tasks.py --check && python tools/gen/build_tasks.py` 重生成——禁止手工编辑 `tasks/*.md` 与 `scripts/verify/task-*.sh`。
2. **防相互干扰的执行**：实施任一 TASK 时，遵守 §27.3 红线（只调依赖 `include/`、不啃 `src/`）；验收脚本的 `scan_forbidden` 会在本地硬性拦截违规。
3. **缺口已补**：Social / ControlService / 集成回归三块现在都有可执行规格，可纳入正常实施序列。
4. **Phase 2 工作**：Live Scene Migration 走 RFC 流程，不在第一版范围内。

---

## 7. 改动文件清单

- `mmorpg_tasks/tools/gen/data_d.py` —— 新增 TASK-039/040/041、TASK-038 依赖补入、修复 §7 渲染 `"`
- `mmorpg_tasks/tools/gen/build_tasks.py` —— 新增 §27（导出/消费/边界/扩展）、模块边界静态扫描、Phase 倒置检测跳过汇点
- `mmorpg_tasks/tasks/TASK-000.md … TASK-041.md` —— 全量重生成（42 份）
- `mmorpg_tasks/scripts/verify/task-*.sh` —— 全量重生成（42 份，含边界扫描）
- `mmorpg_tasks/docs/rfc/scene-live-migration.md` —— 新增 RFC
- `mmorpg_tasks/README.md` —— 任务数 39→42、§6 章节 27→28、新增 §10 完善说明
