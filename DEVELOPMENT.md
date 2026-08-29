# DEVELOPMENT.md

> 模块开发规范。所有新增模块必须遵守本文件；既有规则与 `PROJECT_REQUIREMENTS.md` 冲突时以冻结规范为准。

## 1. 目录模板（每个模块）

```
<ModuleName>/
├── include/<ModuleName>/...   公开头（接口契约冻结点）
├── src/...                     实现（禁止被下游 #include）
├── tests/...                   GoogleTest 单测 + 集成测试
├── benchmark/...               真实 Benchmark（输出 key=value）
├── docs/...                    README / INTERFACE / DEPENDENCY / PERFORMANCE / TEST
└── CMakeLists.txt
```

## 2. 模块必备五文档

| 文档 | 内容 |
|---|---|
| README | 定位 / 范围 / 不做什么 |
| INTERFACE | 公开接口与冻结契约 |
| DEPENDENCY | 上游依赖（仅通过声明接口调用） |
| PERFORMANCE | Benchmark 实测数字与基准 |
| TEST | 测试覆盖与 Failure Test 场景 |

## 3. 错误码表（统一）

全项目统一 `mmo::core::Result<T>` / `ErrorCode` / `Error` 三件套（见 TASK-001）。
标准错误码（9 个）：`OK / InvalidArgument / NotFound / AlreadyExists / PermissionDenied /
ResourceExhausted / Internal / Timeout / Unavailable`。禁止模块自行设计冲突错误体系。

## 4. 日志字段规范

统一 Logger + TraceID 传播（见 TASK-002）。固定字段：
`Timestamp / Level / Service / Module / TraceID / RequestID / PlayerID / SceneID / Message`。
同一请求的全链路日志通过 TraceID 串起来。

## 5. 依赖单向规则

```
Game → Gameplay → Core
```

- 模块 ≠ 进程；同进程模块通过 Interface / Command / Event 通信。
- 下游只能 `#include` 上游 `include/` 下的公开头；禁止 `#include` 上游 `src/` 或内部头。
- 禁止访问依赖模块内部数据（如 `otherModule.internalData`）。
- 禁止循环依赖；新增模块不得破坏既有依赖环。
- 接口在 `STATUS: DONE` 后变更必须走 `version` + 兼容性评估，禁止静默改签名。

## 6. 扩展性约束

- 新增同类能力（新 Command / Event / Scene 类型 / 模块）走**注册表 / ID 段**机制，禁止 `switch` 硬编码穷举。
- 跨模块扩展点统一用抽象（C++ Interface / Command / Event），新增实现**不得修改既有任务文件**。
- 协议 / 接口变更必须带 `version` 字段并向下兼容。

## 7. vcpkg 依赖策略（增量）

- manifest mode，baseline `aae277ac`，triplet `x64-mingw-dynamic`。
- **依赖按任务增量加入**，禁止一次性全上（TASK-000 §15.11）。
- 空骨架阶段 `vcpkg.json` 依赖为空；首个真正需要三方库的任务是 TASK-001（core 基础，可能引入 fmt）。
- protobuf / grpc / flatbuffers 在 TASK-005 / TASK-006 引入；因 GFW 屏蔽 GitHub，vcpkg 联网安装需在可访问 registry 的环境执行，本地构建以「按需声明、离线可绿」为原则。
- 任何新增依赖必须记录到本文件并说明用途模块。

## 8. 构建与验收

- 选项：`MMORPG_BUILD_TESTS` / `MMORPG_BUILD_BENCHMARKS` / `MMORPG_ENABLE_LUA` / `MMORPG_BUILD_CLIENT`。
- 配置：`cmake -S . -B build/<Type> -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`
- **可信验收 = 本地**。CI 仅 configure+build+ctest，结论以本地为准（`docs/ci-status.md`）。
- 一个 Task 未过验收脚本（`scripts/verify/task-NNN.sh`）禁止标记 DONE，禁止进入下一 Task。

## 9. 提交规范

- Conventional Commits；scope = 模块名；一个 TASK 一次独立提交。
- 正文必须含**实测数字**，禁止「性能良好 / 已优化」等不可验证描述。
- 推送走 `git@github.com:22:shengmingaini/CAMI.git`（GFW 屏蔽 ssh.github.com，必须 22 端口）。
