---
TASK-ID: TASK-000
NAME: 项目初始化与仓库规范
PHASE: Phase 0 · 工程基础
MODULE: build / repo
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: 无
---

# TASK-000 · 项目初始化与仓库规范

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-000` |
| NAME | 项目初始化与仓库规范 |
| PHASE | Phase 0 · 工程基础 |
| MODULE | `build / repo` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | 无 |

---

## 1. Objective

建立整个项目的统一工程规则与空工程骨架：目录结构、根 CMake 工程、Debug/Release 双构建、基础 CI、以及 PROJECT_REQUIREMENTS.md / ARCHITECTURE.md / DEVELOPMENT.md 三份最高级规范文档。本任务**不实现任何游戏功能**。

## 2. Dependencies

无前置任务（本任务为 Phase 起点）。

## 3. Module

`build / repo`

## 4. State Owner（状态归属）

仓库与构建配置本身无运行时状态。PROJECT_REQUIREMENTS.md 是冻结规范，Owner 为 @技术总监，变更必须走 RFC + 人工批准；任何 Agent 与开发成员不得直接改写。目录骨架与 CMake 选项由本任务独占定义，后续任务只能在其约束内增量添加。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

本实施清单中的《Project Requirements V1.0 — Frozen Architecture》全文；本地 MinGW MSYS2 g++ 工具链；vcpkg baseline aae277ac

## 6. Output

可编译的空工程；冻结的架构与开发规范文档；CI 可运行的构建流水线

## 7. Public Interface

无业务接口。仅对外暴露构建入口：`cmake -S . -B build/<Type> -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`。

## 8. Data Model

无业务数据结构。仓库元数据：vcpkg.json（manifest mode，baseline aae277ac）

## 9. Thread Model

无

## 10. Hot Path

**NO**


## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- CMakeLists.txt
- vcpkg.json
- .gitignore
- .editorconfig
- cmake/*.cmake
- engine/
- server/
- game/
- client/
- protocol/
- database/
- scripting/
- tests/
- tools/
- docs/
- task/
- scripts/
- .github/workflows/

## 15. Implementation Steps

1. 初始化 Git 仓库：`git init -b main`，写入 .gitattributes（*.sh text eol=lf，*.md text eol=lf，*.cpp text）
2. 建立根目录结构 engine / server / game / client / protocol / database / scripting / tests / tools / docs / task / scripts，每个目录放 .gitkeep 与 README.md 占位
3. 写 README.md：项目定位（类大型 MMORPG 服务器框架，50k CCU 扩展路线）、进程拓扑图、构建前置条件、快速开始三条命令
4. 写 LICENSE（MIT 或 Apache-2.0，二选一后写死，禁止留 TODO）
5. 写 PROJECT_REQUIREMENTS.md：**原样固化**《Project Requirements V1.0》，并在文件头标注「本文件冻结，修改需 RFC + 人工批准」
6. 写 ARCHITECTURE.md：四进程（Gateway/GameNode/DataService/ControlService）、GameNode 内部模块树、Command/Query/Event 三通信模型、状态 Ownership 表、Tick 阶段划分
7. 写 DEVELOPMENT.md：目录模板（ModuleName/{include,src,tests,benchmark,docs,CMakeLists.txt}）、模块必备五文档（README/INTERFACE/DEPENDENCY/PERFORMANCE/TEST）、错误码表、日志字段规范、依赖单向规则（Game→Gameplay→Core）
8. 创建根 CMakeLists.txt：C++20、`add_subdirectory` 各顶层目录、选项 `MMORPG_BUILD_TESTS=ON`、`MMORPG_BUILD_BENCHMARKS=ON`、`MMORPG_ENABLE_LUA`、`MMORPG_BUILD_CLIENT`
9. 创建 cmake/ 模块：CompilerWarnings.cmake（-Wall -Wextra -Wpedantic，MSVC 用 /W4）、Sanitizers.cmake（Debug 下可选 ASan）、vcpkg triplet 固定 x64-mingw-dynamic
10. 配置 Debug / Release 两套构建：Debug 关闭优化保留断言，Release 开 -O2 且保留 NDEBUG 断言开关可回退
11. 写 vcpkg.json：name/version/builtin-baseline=aae277ac，第一版依赖先只放 fmt、gtest、benchmark、protobuf、grpc、flatbuffers（按需裁剪，禁止一次性全上）
12. 写 .gitignore：build/ out/ vcpkg_installed/ *.o *.obj *.exe *.pdb .vs/ .idea/ .cache/
13. 写 .github/workflows/build.yml：ubuntu-latest + windows-latest 两档，仅做 configure+build+ctest，**不作为可信验收依据**（可信验收=本地）
14. 建立 task/ 目录与脚本入口 scripts/verify/_common.sh、scripts/task-done.sh（此时还是空工程，脚本只做占位与占位校验）

## 16. Unit Test

无业务代码，只验证构建系统：cmake configure 成功、Debug 与 Release 各成功构建一次、`ctest` 可执行且返回「No tests were found」而非崩溃

## 17. Integration Test

git clone 到临时目录后按 README 三条命令可复现构建；CI workflow 语法通过 actionlint（本地可选）

## 18. Benchmark

无

## 19. Failure Test

故意写入一个语法错误源文件，验证构建**会失败**（防止 CI 假绿）；删除 vcpkg toolchain 参数，验证 configure 报错而非静默跳过

## 20. Acceptance Criteria

1. `cmake -S . -B build/Debug -DCMAKE_BUILD_TYPE=Debug` 与 Release 均 configure 成功
2. Debug / Release 两套均 `cmake --build` 成功，退出码 0
3. `ctest --test-dir build/Release` 可执行且不报错
4. README.md / LICENSE / PROJECT_REQUIREMENTS.md / ARCHITECTURE.md / DEVELOPMENT.md 五份文件存在且非空
5. PROJECT_REQUIREMENTS.md 首屏含「冻结」声明与 RFC 变更流程
6. 根目录下**不存在**任何游戏功能代码（src 中无 combat/scene/aoi 等字样），用 grep 验证
7. .gitignore 生效：`git status --short` 不出现 build/、vcpkg_installed/
8. CI workflow 在 GitHub Actions 上至少触发一次并输出构建日志（失败不影响本地验收结论，但必须记录结论到 docs/ci-status.md）

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止实现任何游戏功能（战斗/移动/AOI/网络等）
- 禁止在 vcpkg.json 里一次性声明全部依赖，按需增量添加
- 禁止把 CI 结果当作验收通过依据
- 禁止提交 build/、vcpkg_installed/ 等构建产物
- 禁止在没有 LICENSE 的情况下初始化仓库

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

无性能目标。构建耗时基线记录到 docs/build-baseline.md（冷构建/增量构建各一次），作为后续编译性能回归参照。

## 23. Deliverables

- README.md
- LICENSE
- PROJECT_REQUIREMENTS.md
- ARCHITECTURE.md
- DEVELOPMENT.md
- CMakeLists.txt
- vcpkg.json
- .gitignore
- .gitattributes
- cmake/*.cmake
- .github/workflows/build.yml
- docs/build-baseline.md
- docs/ci-status.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-000.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-000.sh
bash scripts/verify/task-000.sh
```

脚本执行的检查项：

1. 交付物存在性检查（12 项）
2. 静态红线扫描：`engine` 内禁止出现 /\bstd::cout\s*<</
3. 静态红线扫描：`server` 内禁止出现 /\bstd::cout\s*<</
4. 静态红线扫描：`engine` 内禁止出现 /\bprintf\s*\(/
5. CMake configure + 编译（Debug + Release 双构建）
6. ctest 过滤执行：`-R Core`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-000

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
build(build): 项目初始化与仓库规范

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-000.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-000
EOF

# 3) 推送：GFW 屏蔽 ssh.github.com，必须走 22 端口
git push git@github.com:22:shengmingaini/CAMI.git main
```

提交规范：

- 类型：`feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build` / `chore`（本任务建议 `build`）
- **一个 TASK 一次独立提交**，禁止把多个 TASK 合并提交
- 正文必须包含实测数字，禁止「性能良好 / 已优化」这类无法验证的描述
- 未通过验收脚本禁止提交，禁止 `--no-verify` 绕过

## 26. Codex Execution Rules

1. 读规范：先读 `PROJECT_REQUIREMENTS.md` 与本任务涉及章节，架构冻结，不得自行推翻。
2. 读任务：完整读完本文件全部章节再动手，禁止只看 Objective 就开始写。
3. 查依赖：确认 无前置 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-000.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- 无前置依赖，不消费任何上游接口。

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
