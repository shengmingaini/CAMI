# engine/rpc · DEPENDENCY

## 上游（本任务消费，禁止绕过）
- `mmo::core_error`（TASK-001）：Result / Error / ErrorCode
- `mmo::core_log`（TASK-002）：Logger（拦截器日志）
- `mmo::core_uuid`：idempotency_key 生成
- `mmo::rpc_service`（TASK-005 protocol）：echo/health 生成的 gRPC stub

## 第三方
- gRPC 1.82.0（MSYS2 mingw64，gRPC::grpc++）+ protobuf（DPROTOBUF_USE_DLLS）

## 下游（谁可依赖本模块）
- 任意需要跨进程 RPC 的模块（gateway / dataservice / control 等），只允许
  `#include "mmo/rpc/*.h"` 公开头；禁止 include 本模块 `src/`。
- 全仓约束：engine/rpc 之外禁止 `grpc::` 直接调用。
