# engine/rpc · INTERFACE

> **首页红线：战斗 Tick（Combat / Movement / AOI / Buff）内禁止发起同步 RPC。**
> RPC 只允许用于跨进程、低频、管理面与数据面调用（PROJECT_REQUIREMENTS §5.3）。

## 公开接口（冻结契约）

### RpcOptions（`rpc_options.h`）

| 字段 | 默认 | 说明 |
|---|---|---|
| `timeout_ms` | 500ms | 单次尝试超时；**0 = INVALID_ARGUMENT**（禁止永不超时） |
| `retry_on_unavailable` | true | 允许重试的总开关 |
| `max_retries` | 2 | 最大重试次数 |
| `backoff_base_ms` | 20ms | 指数退避基准；`min(base<<n, 1s) × jitter[0.8,1.2]` |
| `idempotent` | false | **非幂等调用重试次数恒为 0（红线）** |

### GrpcChannelPool（`grpc_channel_pool.h`）

```cpp
static Result<std::shared_ptr<GrpcChannelPool>> Create(size_t per_target = 4);
Result<std::shared_ptr<grpc::Channel>> Get(std::string_view target);  // 空轮询复用
void HealthCheck();  // TRANSIENT_FAILURE 触发 WaitForConnected(100ms)；SHUTDOWN 重建
```

- channel 生命周期由池独占；调用方**禁止** `grpc::CreateChannel`。
- 建连参数固定 `grpc.enable_http_proxy=0`：内部 RPC 不得经过系统/企业 HTTP 代理
  （实测企业代理会把 `127.0.0.1` 回环拦成 HTTP 502，见 git 记录）。

### GrpcClient<Stub>（`grpc_client.h`）

```cpp
GrpcClient(std::shared_ptr<GrpcChannelPool>, std::string target, StubFactory);
template <typename Req, typename Resp>
Result<Resp> Call(grpc::Status (Stub::*)(grpc::ClientContext*, const Req&, Resp*),
                  const Req&, RpcOptions,
                  const std::function<void(grpc::ClientContext&)>& ctx_hook = nullptr);
```

- 每次尝试独立 deadline；metadata 透传 `trace_id`（取线程 LogContext）。
- 重试：仅 `idempotent=true` 且错误 ∈ {TIMEOUT, BUSY, RATE_LIMITED}；指数退避 + ±20% 抖动 + 上限 1s；**所有尝试共用同一个 idempotency_key**（非幂等调用不生成）。
- stub 按 channel 缓存复用（unary 调用线程安全）。
- `ctx_hook` 供取消等高级用法，禁止借它建连/绕过封装。

> **任务书 §7 偏差说明**：任务书把方法指针返回值写成 `Resp`，真实 gRPC stub
> 返回 `grpc::Status`；本实现按真实签名 `grpc::Status (Stub::*)(...)` 落地。

### GrpcServer（`grpc_server.h`）

```cpp
struct Config { std::string listen_addr; uint32_t max_threads{4};
                size_t max_recv_msg_size{4MB}; uint32_t rate_limit_rps{0}; };
static Result<std::unique_ptr<GrpcServer>> Create(Config, std::vector<grpc::Service*>);
Result<void> Start();      // 绑定失败 → BUSY
void Shutdown(DurationMs grace = 5000ms);  // 优雅关闭：grace 内等在途请求
int bound_port() const;    // listen_addr 端口 0 时回填实际端口
```

> **任务书 §7 偏差说明**：`max_recv_msg_size` 任务书标注 `DurationMs` 属数据源
> 笔误（消息大小为字节数），本实现取 `size_t`，默认值 4MB 不变。

内置三拦截器（服务端同步 API）：
1. **TraceID 透传**：`POST_RECV_INITIAL_METADATA` 提取客户端 `trace_id` metadata；
2. **日志**：`PRE_SEND_STATUS` 记录 method / latency_us / status / trace_id（走 `mmo::core::Logger`）；
3. **限流**：`rate_limit_rps>0` 时启用令牌桶（burst=rate），耗尽在 `PRE_SEND_STATUS`
   用 `ModifySendStatus` 改写为 `RESOURCE_EXHAUSTED`。
   说明：gRPC 同步服务端拦截器无法在 handler 执行前短路，拒绝发生在响应阶段，
   handler 仍会执行一次（框架验证语义，业务限流请配合 handler 内预检）。

### 错误映射（`status_mapping.h`，全仓唯一实现）

| gRPC code | core::ErrorCode |
|---|---|
| OK | OK |
| INVALID_ARGUMENT | INVALID_ARGUMENT |
| NOT_FOUND | NOT_FOUND |
| DEADLINE_EXCEEDED | TIMEOUT（可重试） |
| UNAVAILABLE | BUSY（仅幂等可重试） |
| RESOURCE_EXHAUSTED | RATE_LIMITED（仅幂等可重试） |
| UNAUTHENTICATED | UNAUTHORIZED |
| FAILED_PRECONDITION | VERSION_CONFLICT |
| INTERNAL / UNKNOWN | INTERNAL_ERROR |
| 表外兜底 | CANCELLED / ABORTED / UNIMPLEMENTED / DATA_LOSS / DO_NOT_USE → INTERNAL_ERROR；PERMISSION_DENIED → UNAUTHORIZED；OUT_OF_RANGE → INVALID_ARGUMENT |

**平台差异（Windows 实测）**：连本机未监听端口时 gRPC 子通道持续重连直至
deadline，返回 DEADLINE_EXCEEDED→TIMEOUT（Linux 预期为 UNAVAILABLE→BUSY）；
两者均满足 §19「timeout 内明确错误、不崩溃」意图。
客户端主动取消（TryCancel）→ CANCELLED → INTERNAL_ERROR（受控 ErrorCode 枚举
无 CANCELLED，语义以 message 区分，禁止为此扩顶层枚举）。

### 客户端指标（`rpc_metrics.h`，为 TASK-039 预留）

`RpcMetrics::Instance()`：per-method（Stub 成员指针为键）的 calls / errors /
retries / avg_latency_us；`Snapshot()` 供周期上报。
