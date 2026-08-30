# engine/rpc · RPC Framework（gRPC 统一封装）

跨进程统一 gRPC 封装：GrpcChannelPool（连接复用）/ GrpcClient（超时·取消·重试·退避）/
GrpcServer（配置化·优雅关闭·三拦截器）/ MapStatus（唯一错误映射）/ RpcMetrics（TASK-039 预留）。

- 文档：INTERFACE（契约+偏差记录）/ PERFORMANCE（实测）/ DEPENDENCY / TEST
- 红线：**战斗 Tick 禁止同步 RPC**；非幂等禁重试；禁止每次调用建连；engine/rpc 之外无 grpc:: 直接调用。
- 验证：`bash scripts/verify/task-006.sh`（ctest -R Rpc + bin/rpc_bench --iterations 100000）
