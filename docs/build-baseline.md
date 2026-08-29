# Build Baseline（编译性能基线）

> 用途：作为后续编译性能回归参照。每次新增模块/依赖后在此追加记录，禁止估算，必须贴真实数字。

## 环境

| 项 | 值 |
|---|---|
| 编译器 | MinGW-w64 g++ 16.1.0 (C++20) |
| 构建系统 | CMake 4.4.2 + Ninja |
| vcpkg | manifest mode, baseline `aae277ac`, triplet `x64-mingw-dynamic` |
| 依赖 | 空骨架阶段 `vcpkg.json` 依赖为空（离线可绿；三方库按任务增量加入） |

## 测量（TASK-000 空骨架，2026-08-30）

| 动作 | Release | Debug |
|---|---|---|
| cmake configure | 7.5 s | 5.5 s |
| cmake --build（首轮） | ninja: no work to do（无编译单元） | 同左 |
| ctest | No tests were found | No tests were found |

说明：空骨架尚无任何编译单元，构建耗时近似 0。首个真正加入编译源码的任务是 **TASK-001（core 基础）**，届时此处追加真实编译/链接耗时作为回归基准。

## Anti-False-Green 验证（§19 Failure Test）

临时在 `tests/` 放入必然编译失败的源文件，构建**失败**（退出码 1，c++.exe 报 `error: invalid use of 'this'` 等真实错误）；删除后重新 configure+build 恢复绿色。证明构建系统会真实失败，不存在“假绿”。
