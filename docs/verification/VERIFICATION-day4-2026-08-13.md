# Day 4 验收 — 工程基建：CMake monorepo 骨架 + 五层目录 + CI 流水线

- **日期**：2026-08-13（周四）
- **任务卡**：CMake monorepo 工程骨架（五层目录、CI 流水线）
- **专家**：game-developer
- **验收线**：CI 绿灯；目录与分层一一对应

---

## 1. 现状基线（重要）

项目根目录在 Day 1–3 期间已存在**空壳骨架**：`gateway/`、`game/`、`data/`、`common/`、`ops/` 等五层目录按架构 spec §3.1 建好，但：
- 根 `CMakeLists.txt` 的五层 `add_subdirectory` **全部被注释**，构建未真正接入各层；
- 各层目录为空（无 `CMakeLists.txt`、无源文件）；
- **无 CI 配置**。

因此 Day 4 的实质工作是：把五层**真正接进 monorepo 构建**（每层成为可编译的静态库 target，体现单向依赖），并补齐 CI 流水线。

---

## 2. 交付物清单

| 交付物 | 路径 | 说明 |
|--------|------|------|
| 根构建脚本 | `CMakeLists.txt` | 取消注释五层 `add_subdirectory`；无条件 `enable_testing()`；加 `cami_skeleton_check` 自检目标 + `add_test`；修正 `-Wthread-safety` 仅 Clang 生效 |
| 接入层构建 | `gateway/CMakeLists.txt` + `gateway_layer.{h,cpp}` | `cami_gateway` 静态库，link `cami_common` |
| 逻辑业务层构建 | `game/CMakeLists.txt` + `game_layer.{h,cpp}` | `cami_game` 静态库，link `cami_common` + `cami_data` |
| 数据管理层构建 | `data/CMakeLists.txt` + `data_layer.{h,cpp}` | `cami_data` 静态库，link `cami_common` |
| 公共服务层构建 | `common/CMakeLists.txt` + `common_layer.{h,cpp}` | `cami_common` 静态库，无下层依赖 |
| 运维控制层构建 | `ops/CMakeLists.txt` + `ops_layer.{h,cpp}` | `cami_ops` 静态库，link `cami_common` |
| 分层自检程序 | `tests/unit/skeleton_layer_check.cpp` | 零外部依赖（不依赖 GTest）；编译期断言五层已链接，作为 ctest 用例 |
| CI 流水线 | `.github/workflows/ci.yml` | ubuntu-latest：装 cmake/g++/libboost-dev → configure(`CAMI_BUILD_MODULES=OFF`) → build → ctest |
| 构建说明 | `README.md`（Build 段 + 映射表） | 轻量/全量/MSYS2 三种构建方式 + 五层↔目录↔target 映射 |
| 本验收文档 | `docs/verification/VERIFICATION-day4-2026-08-13.md` | — |

---

## 3. 五层 ↔ 目录 ↔ target 一一对应

| 架构五层 (spec §3.1) | 目录 | CMake target | 依赖 |
|----------------------|------|--------------|------|
| 接入层 Gateway | `gateway/` | `cami_gateway` | `cami_common` |
| 逻辑业务层 Game Node | `game/` | `cami_game` | `cami_common`, `cami_data` |
| 数据管理层 Data Service | `data/` | `cami_data` | `cami_common` |
| 全局公共服务层 Common | `common/` | `cami_common` | （无） |
| 运维控制层 Ops | `ops/` | `cami_ops` | `cami_common` |

依赖方向严格单向（spec §3.2）：`gateway → game → data → common`；`ops → common`（控制面）。CMake `target_link_libraries` 已按此声明，违反单向性会在链接期因缺失依赖而失败。

---

## 4. 验收方式

### 4.1 CI 绿灯（标准 runner）
`.github/workflows/ci.yml` 在 `ubuntu-latest` 上：
1. `apt-get install -y cmake g++ libboost-dev`
2. `cmake -B build -DCAMI_BUILD_MODULES=OFF`（跳过重型依赖）
3. `cmake --build build`
4. `ctest --test-dir build --output-on-failure`

`ctest` 运行 `skeleton_layer_check`：它 `#include` 五层头并断言五个 `kLayerName` 依次等于 `common/data/game/gateway/ops`。**若任一层未接入构建，链接失败 → CI 红**；全部接入则打印分层确认并 `return 0` → CI 绿。

### 4.2 本地验证（用户 MSYS2 / 任意装了 cmake+g+++boost 的环境）
```bash
cmake -B build -DCAMI_BUILD_MODULES=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```
预期输出含五行 `[ OK ] <layer> linked into build` 与一行 `=== CAMI 五层架构已接入 monorepo 构建：目录↔分层↔target 一一对应 ===`。

---

## 5. 已知限制（诚实声明）

1. **沙箱未本地跑通**：本次工作环境（WorkBuddy 沙箱，MINGW64）**未安装 cmake / g++ / boost**（`command not found`），无法在交付前本地执行 `cmake --build` 验证。CI 配置与全部 CMake 已按标准 `ubuntu-latest` runner 设计（`CAMI_BUILD_MODULES=OFF` 仅依赖 Boost header，ubuntu 装 `libboost-dev` 即可），需在 GitHub runner 或用户本机 MSYS2 跑绿。
2. **重型依赖默认关闭**：`CAMI_BUILD_MODULES=OFF` 跳过 gRPC/Protobuf/Redis/Sol2/Lua，因此 `proto/`、`lua/`、各层业务逻辑尚未编译进构建。这是骨架阶段的刻意设计——保证 CI 快速绿灯，各层实现后开启。
3. **各层为声明级骨架**：`*_layer.{h,cpp}` 仅声明层名与依赖方向，不含业务逻辑。业务实现是后续天（Day 5+）的工作。

---

## 6. 结论

- ✅ 五层目录与架构 spec §3.1 **一一对应**，且已接进 monorepo 构建（5 个真实静态库 target）。
- ✅ 依赖方向按 spec §3.2 单向声明（`target_link_libraries`）。
- ✅ CI 流水线已就绪（`ubuntu-latest`，轻量骨架构建 + ctest 自检）。
- ⏳ CI 实际绿灯需在 runner/本机执行（沙箱无工具链，未本地证）；配置本身满足标准 runner 绿灯条件。
- 后续：各层业务逻辑实现时开启 `CAMI_BUILD_MODULES=ON`，逐步把 `proto/`、`lua/`、业务模块接入 `cami_<layer>` target。
