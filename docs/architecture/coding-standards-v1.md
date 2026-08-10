# CAMI 编码规范 v1

> 适用范围：CAMI 全量 C++17 代码（gateway / game / data / common / ops 五层）
> 生效：Day 5（2026-08-14）起。后续版本以 `coding-standards-vN.md` 递增。
> 目标：可读、正确、可维护、可扩展；热路径高性能；与既有 ADR-002/ADR-012 及五层架构一致。

## 1. 总则

- **清晰优先于聪明**：能被同事一天后读懂的代码，胜过炫技的紧缩写法。
- **正确优先于性能**：先正确，再用基准（见 §7）驱动优化；禁止未经基准的"优化"。
- **RAII 优先**：所有资源（内存、锁、文件、连接）用对象生命周期管理，禁止裸 `new`/`delete`、裸 `malloc`/`free`。
- **const 默认**：能 const 的地方都 const；成员函数不改状态就标 `const`。
- **小步提交、机器可验证**：每层有 selfcheck，CI 必须绿灯，吞吐/延迟有基准背书。

## 2. 命名

| 类别 | 规则 | 示例 |
|------|------|------|
| 类型/类/结构体/枚举 | `PascalCase` | `EventBus`, `MobKilledEvent`, `Channel` |
| 函数/变量/成员 | `snake_case` | `publish`, `handled_count`, `enqueue_pos_` |
| 命名空间 | 全小写 | `cami`, `cami::common` |
| 编译期常量（constexpr/全局） | `k` 前缀 + `PascalCase` | `kLayerName`, `kDependsOn` |
| 成员变量 | 末尾下划线 | `queue_`, `handler_mtx_` |
| 局部静态/文件内 | 同上或普通 snake_case | — |
| 宏 | `UPPER_SNAKE_CASE`（尽量少用） | `CAMI_BUILD_MODULES` |
| 模板参数 | `PascalCase` 或单大写字母 | `template <typename T>` |

- 事件名用过去式/名词：`MobKilledEvent`、`ItemObtainedEvent`（与 ADR-012 发布事件表一致）。
- 通道名语义化、稳定：`"mob_killed"`、`"player_enter_area"`。

## 3. 包含与头文件

- 一律用 `#pragma once`（项目统一，不用传统 include guard）。
- 项目内包含写**相对仓库根的全路径**，避免歧义：
  - ✅ `#include "common/event_bus/mpmc_queue.h"`
  - ❌ `#include "mpmc_queue.h"`
- 系统/三方库用 `<>`；项目用 `""`。
- **IWYU（Include What You Use）**：每个文件只 include 它直接用到的符号；不在头里靠别的头间接引入。
- 头文件只forward-declaration 能解决的不 include；`.cpp` 里补完整 include。
- 禁止在头文件中 `using namespace`（污染所有包含者）。

## 4. 错误处理

- **可预期失败**用返回值 / `std::optional<T>` / `std::expected<T,E>`（C++23 前可用 `absl::Status` 或项目自封装 `Result<T>`），不要靠异常穿越模块边界。
- **真正异常**（`std::bad_alloc`、逻辑不可能）才用异常，且只在能安全展开的地方捕获。
- 禁止吞异常：`catch (...) {}` 需有日志与恢复，否则不允许。
- 配置/启动类致命错误：早失败（`std::terminate()` 或返回非 0），不要带病运行。
- 日志：统一经 `common` 的结构化日志（spdlog 接入前用临时桩，禁止 `printf` 散落业务代码——**基准/selfcheck 除外**）。

## 5. 并发（重点）

- **热路径优先无锁**：高频消息/事件传递用 `common/event_bus` 的 MPMC 无锁队列，不要为了"简单"用 `std::mutex` 包整个热路径。
- **内存序**：
  - 默认用 `std::memory_order_acquire` / `release` 配对建立 happens-before；
  - 位置计数器、纯计数器等无需同步数据可见性的用 `relaxed`；
  - **禁止无理由用 `seq_cst`**（它是默认序，但应显式写清意图，仅在确实需要全局顺序时用）。
- **伪共享**：跨线程高频写的变量（原子计数器、标志）用 `alignas(64)` 隔离到独立缓存行（见 `mpmc_queue.h`）。
- **冷路径可加锁**：订阅注册、配置热更等低频操作，用 `std::mutex` 完全可接受。
- **禁止**：裸 `pthread_*`、裸 `volatile` 当同步、在未加锁下跨线程共享非原子非只读状态。
- 锁的粒度：尽量用 `std::scoped_lock` / `std::lock_guard`，禁止手动 `lock()`/`unlock()` 配对易漏。

## 6. 资源与所有权

- 所有权用智能指针：`std::unique_ptr`（独占）、`std::shared_ptr`（共享，谨慎用——引用计数本身有开销）。
- 禁止返回/传递裸 owning 指针；接口优先按值或 `const T&` 传参。
- 数组用 `std::vector` / `std::array` / `std::unique_ptr<T[]>`（MPMC 队列即用此），禁止 C 风格 `new T[n]` + `delete[]`。
- 文件/套接字/连接：用 RAII wrapper（如 `boost::asio` 的 socket 对象、自定义 `ScopeGuard`）。

## 7. 性能与基准

- 任何"性能关键"改动必须配基准：放 `benchmark/`，用 `cmake -DCAMI_BUILD_BENCHMARK=ON` 可构建。
- 基准要可复现：固定线程数、运行时长、队列容量（2 的幂），输出 Mmsg/s 或 ops/s。
- 验收阈值写进任务卡，实测数值写进周报/验收文档。
- 不凭感觉优化；优化前后都跑基准对比。

## 8. 模块边界（与 ADR 对齐）

- **ADR-002**：`character` 独占 `Player`；其他模块只 `const Player&` 读 + 调 `ApplyXxx()` 写；`combat`/`quest`/`economy`/`social` 不拥有 `Player`。
- **ADR-012**：内部 EventBus 事件名 ≠ 网络 FlatBuffers 消息名；事件总线只做进程内解耦，不生成网络协议；`economy` 为发奖唯一出口（`quest → economy → character`）。
- **区域硬约束**：区域相互独立、仅经门连接、副本团本即独立区域（`config_zones.proto`）。
- 五层单向依赖不可反向：`gateway → game → data → common`；`ops → common`。禁止上层 `#include` 下层反向、禁止循环依赖。

## 9. 构建与 CMake

- 每层是 `cami_<layer>` STATIC 库，源码在 `<layer>/`。
- 优先 `target_include_directories` / `target_link_libraries`（target 级），不用 `include_directories` 全局污染（根 CMakeLists 的项目根 include 为历史兼容，新代码勿扩）。
- 跨平台链接：任何用 Boost.Asio / 原生 socket 的 Windows 目标，必须
  `target_link_libraries(... ws2_32 mswsock)` 且用 `if(WIN32)` 守卫（Linux socket 在 libc，CI 不报错但本地 MinGW 必踩）。
- 编译选项由根 CMake 统一（`-Wall -Wextra -Wpedantic`），新层勿自行加全局 `-Werror` 之外的脏选项。

## 10. 测试

- 每层提供 `selfcheck()` 类函数，被 `tests/unit/skeleton_layer_check.cpp` 或各层单测调用，作为"已接入且功能正常"的机器证据。
- 性能相关用基准（§7），不稳定性用例不进 CI 定时门禁（避免 flaky）。
- 提交前本地必须：`cmake --build build -j && ctest --test-dir build --output-on-failure` 全绿（在 MINGW64 终端，非 base MSYS）。

## 11. 格式化（建议）

- 2 空格缩进，100 列宽，UTF-8，LF 换行。
- 建议接入 `.clang-format`（基于 LLVM 风格微调）；提交前 `clang-format` 一遍。
- 头文件顺序：自身对应头 → C 系统 → C++ 标准 → 三方 → 项目内。

---
*本规范随项目演进而迭代；v1 聚焦五层架构落地后的基础契约，重并发与模块边界。*
