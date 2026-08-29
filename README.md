# MMORPG Server Framework

> 模块化、低资源、可水平扩展的 MMORPG 后端 + 客户端框架。
> 目标架构：**50,000 CCU**；少进程、强模块、统一接口、统一通信、单一状态 Owner、热路径零阻塞。

## 进程拓扑

```
                  Client
                     │
                 Protocol
                     │
                  Gateway
                     │
                   gRPC
                     │
        ┌────────────┼────────────┐
        │            │            │
     GameNode     GameNode     GameNode      ← 实时游戏逻辑（Scene/AOI/Movement/Combat/...）
        │            │            │
     ┌──┴────────────────────────┐
     │       Game Modules        │          ← 同进程模块，非独立进程
     └────────────┬──────────────┘
                  │
             DataService
              ┌───┴───┐
              │       │
            Redis   MySQL

             ControlService                    ← 节点管理/配置/版本/热更/部署（不拥有游戏状态）
```

四类核心进程：**Gateway / GameNode / DataService / ControlService**。
GameNode 内部模块（Scene / AOI / Movement / Combat / Role / Inventory / Quest / Social / Economy / Instance / Lua）默认运行于同一进程，通过公开 Interface / Command / Event 通信，**禁止为了模块化而把同进程高频调用转为网络 RPC**。

## 构建前置条件

| 组件 | 版本 / 说明 |
|---|---|
| 编译器 | MinGW-w64 MSYS2 **g++ 16.x**（C++20），或 MSVC 17+ |
| CMake | ≥ 3.25 |
| vcpkg | manifest mode，baseline `aae277ac`，triplet `x64-mingw-dynamic` |
| 本地运行时 | Redis 5.x、MariaDB/MySQL 8（开发期可选，见 `.workbuddy/tools/`） |

> 可信验收 = **本地**编译/测试/压测。CI 仅做 configure+build+ctest，不作为验收通过依据（详见 `docs/ci-status.md`）。

## 快速开始（三条命令）

```bash
# 1) 配置（Release）
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# 2) 构建
cmake --build build/Release -j

# 3) 测试
ctest --test-dir build/Release --output-on-failure
```

开发期启本地 Redis / MariaDB（一键脚本，在用户自己终端运行，避免占用本会话）：

```bash
# 仅本地开发用，非生产
F:/AI/workbuddy/2026-08-29-23-22-17/.workbuddy/tools/start-redis.bat
F:/AI/workbuddy/2026-08-29-23-22-17/.workbuddy/tools/start-mariadb.bat
```

## 最高级规范（冻结）

- `PROJECT_REQUIREMENTS.md` —— **冻结**架构规范，修改必须走 RFC + 人工批准。
- `ARCHITECTURE.md` —— 四进程 / 模块树 / C-Q-E 通信 / 状态 Ownership / Tick 划分。
- `DEVELOPMENT.md` —— 目录模板 / 五文档契约 / 错误码 / 日志字段 / 依赖单向规则。

## 任务执行

任务清单见 `mmorpg_tasks/tasks/TASK-000.md … TASK-041.md`，从 TASK-000 起逐任务执行，
每个任务必须过本地验收脚本（`scripts/verify/task-NNN.sh`）后才标记 `STATUS: DONE` 并进入下一任务。
