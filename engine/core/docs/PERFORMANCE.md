# engine/core · 性能实测（TASK-001）

> 所有数字由 `engine/core/tests/error_bench`（1e7 次迭代）真实测量，机器可读输出见
> `bench/core_error.txt`。验收阈值：`result_ns_per_op ≤ 5`、`alloc_per_fail ≤ 0`。
> 验收脚本以 **Release** 构建运行 benchmark（满足阈值）。

## 环境

- 编译器：g++ (MSYS2 MinGW-w64) 16.1.0
- 构建：CMake 4.4.2 + Ninja，C++20（`-std=gnu++20`）
- 优化：Release `-O3 -DNDEBUG`；Debug `-O0 -g`
- 依赖：纯标准库，vcpkg manifest mode（空依赖，离线构建）

## 实测结果

| 指标 | Release | Debug | 阈值 | 结论 |
|---|---|---|---|---|
| `result_ns_per_op`（Result 构造+析构，1e7 次） | **0.219 ns** | 45.829 ns | ≤ 5 ns | ✅ |
| `alloc_per_fail`（失败路径堆分配次数 / op，1e7 次） | **0** | 0 | ≤ 0 | ✅ |

## 说明

- `result_ns_per_op` 使用 `volatile` 接收结果并以迭代序号作值，确保构造工作不被优化消除，
  为真实可重复测量（基准：约 0.2 ns/op，远低于 5 ns 阈值）。
- `alloc_per_fail = 0`：失败路径使用 32 字节内联 `message` 缓冲，`Error` 复制与 `Result`
  移动均不触发 `operator new`；`AndThen`/`Map` 失败时仅复制 `Error`（仍零分配）。
- `ToString()` 单次：`std::string` 拼接（domain + 名称 + message），无格式化额外拷贝；
  短消息下 O(message_len)，无堆分配。
