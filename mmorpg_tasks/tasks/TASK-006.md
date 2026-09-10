---
TASK-ID: TASK-006
NAME: RPC Framework（gRPC 统一封装）
PHASE: Phase 1 · 统一通信
MODULE: engine/rpc
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-001, TASK-005
---

# TASK-006 · RPC Framework（gRPC 统一封装）

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-006` |
| NAME | RPC Framework（gRPC 统一封装） |
| PHASE | Phase 1 · 统一通信 |
| MODULE | `engine/rpc` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-001`, `TASK-005` |

---

## 1. Objective

封装统一 gRPC 客户端/服务端：超时、取消、重试、错误映射（gRPC status ↔ mmo::core::Error）。**RPC 只允许用于跨进程、低频、管理面与数据面；禁止在战斗 Tick 内使用同步 RPC。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-001` · Core Error / Result 系统
- `TASK-005` · Protocol Schema（Protobuf + FlatBuffers）

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 001 005`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`engine/rpc`

## 4. State Owner（状态归属）

gRPC Channel Pool 由本任务独占维护（连接生命周期、重连、健康检查、超时）。调用方只持有 stub 句柄，禁止自行创建 channel。RPC 层只做搬运，不持有任何游戏状态，不缓存业务数据。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-001 Error/Result；TASK-005 Protobuf 定义（服务定义基于 .proto）

## 6. Output

engine/rpc 模块（GrpcClient / GrpcServer / 拦截器 / 重试策略）+ 端到端测试 + 超时与取消测试

## 7. Public Interface

```cpp
namespace mmo::rpc {
struct RpcOptions {                      // 每次调用必填，禁止用默认值蒙混
  DurationMs timeout_ms{500};
  bool       retry_on_unavailable{true};
  uint32_t   max_retries{2};
  DurationMs backoff_base_ms{20};        // 指数退避，带抖动
  bool       idempotent{false};          // 非幂等调用禁止自动重试
};
class GrpcChannelPool { public:          // 连接复用，禁止每次调用建连
  static Result<std::shared_ptr<GrpcChannelPool>> Create(size_t per_target = 4);
  Result<std::shared_ptr<grpc::Channel>> Get(std::string_view target);
};
template <typename Stub> class GrpcClient {
 public:
  GrpcClient(std::shared_ptr<GrpcChannelPool>, StubFactory);
  template <typename Req, typename Resp>
  Result<Resp> Call(Resp (Stub::*method)(grpc::ClientContext*, const Req&, Resp*),
                    const Req&, RpcOptions);
};
class GrpcServer { public:
  struct Config { std::string listen_addr; uint32_t max_threads{4};
                  DurationMs max_recv_msg_size{4*1024*1024}; };
  static Result<std::unique_ptr<GrpcServer>> Create(Config, std::vector<grpc::Service*>);
  Result<void> Start(); void Shutdown(DurationMs grace{5000});
};
core::Error MapStatus(const grpc::Status&) noexcept;   // 唯一映射入口
}
```

## 8. Data Model

**gRPC status → mmo::core::Error 映射表（唯一真相）**

| gRPC code | mmo ErrorCode | retryable |
|---|---|---|
| OK | OK | - |
| INVALID_ARGUMENT | INVALID_ARGUMENT | 否 |
| NOT_FOUND | NOT_FOUND | 否 |
| DEADLINE_EXCEEDED | TIMEOUT | 是 |
| UNAVAILABLE | BUSY | 是（仅幂等） |
| RESOURCE_EXHAUSTED | RATE_LIMITED | 是（仅幂等） |
| UNAUTHENTICATED | UNAUTHORIZED | 否 |
| FAILED_PRECONDITION | VERSION_CONFLICT | 否 |
| INTERNAL / UNKNOWN | INTERNAL_ERROR | 否 |

## 9. Thread Model

gRPC 使用独立 completion queue 线程池（归属 Worker/Persistence 语义），**不得在 SimulationThread 发起同步调用**。客户端调用为异步 future + 业务线程 await（或投递回调），禁止阻塞 Tick 线程。

## 10. Hot Path

**NO**


## 11. External IO

**YES**

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**YES**

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**NO**


## 14. Files

- engine/rpc/include/mmo/rpc/
- engine/rpc/src/
- engine/rpc/tests/
- engine/rpc/docs/
- protocol/proto/service/

## 15. Implementation Steps

1. 定义 protocol/proto/service/*.proto：先只写 `HealthService` 与 `EchoService`（作为框架验证用），后续 DataService/ControlService 接口由 TASK-026 等增量扩展
2. 实现 grpc_channel_pool.h/.cpp：按 target 复用 channel，含空闲健康检查与重连
3. 实现 grpc_client.h：模板 Call 封装，强制传 RpcOptions，内部完成 deadline 设置、context 取消、重试与退避
4. 实现重试策略：仅当 `idempotent=true` 且 code ∈ {UNAVAILABLE, RESOURCE_EXHAUSTED, DEADLINE_EXCEEDED} 才重试；指数退避 + ±20% 抖动；每次重试必须带**同一个** idempotency_key
5. 实现 status_mapping.h/.cpp：上表映射，禁止其他地方写第二份 switch
6. 实现 grpc_server.h/.cpp：配置化启动、优雅关闭（grace period 内等待在途请求）、最大消息大小限制
7. 实现三个拦截器：TraceID 透传（从 metadata 取/放 trace_id）、日志拦截器（记录 method/latency/status）、限流拦截器（返回 RESOURCE_EXHAUSTED）
8. 实现客户端指标：per-method 的 QPS / 延迟分布 / 错误率 / 重试次数（为 TASK-039 指标采集预留接口）
9. 写测试：正常调用；超时（服务端 sleep 1s、客户端 timeout 200ms → TIMEOUT）；取消（客户端主动 cancel → CANCELLED 映射）；重试（服务端前两次返回 UNAVAILABLE → 第三次成功）；非幂等调用**不允许**重试（断言只调用一次）；错误码映射全覆盖表驱动测试
10. 写 docs/INTERFACE.md 与 docs/PERFORMANCE.md，首页写明「战斗 Tick 禁止同步 RPC」红线

## 16. Unit Test

错误码映射表驱动全覆盖；RpcOptions 校验（timeout=0 → INVALID_ARGUMENT）；退避时间计算；非幂等不重试；channel 池复用（同 target 两次 Get 返回同一对象）

## 17. Integration Test

起一个真实 GrpcServer（本机随机端口），客户端跑：Echo 成功、超时、取消、重试成功、重试耗尽、限流六种场景；TraceID 拦截器验证服务端日志能拿到客户端传入的 trace_id；优雅关闭验证在途请求被等待完成而非硬杀

## 18. Benchmark

bin/rpc_bench：本机回环 1e5 次 Echo，`rpc_p50_us=` / `rpc_p99_us=` / `rpc_qps=`；超时场景 `timeout_overhead_us=`

## 19. Failure Test

服务端未启动：客户端返回 UNAVAILABLE→BUSY 且不崩溃；服务端进程被 kill：客户端在 timeout 内返回，不挂死；网络分区（用 iptables/断网模拟或指向不可达地址）：按退避重试后失败并返回明确错误；消息体超限：服务端拒绝并返回 RESOURCE_EXHAUSTED；重试风暴保护：连续失败时退避上限 1s，禁止无限快速重试

## 20. Acceptance Criteria

1. gRPC status → Error 映射表 100% 覆盖（表驱动单测）且全仓唯一实现
2. 超时、取消、重试成功、重试耗尽四种路径均有集成测试且通过
3. **非幂等调用重试次数 = 0**（单测断言服务端只收到 1 次）
4. TraceID 跨进程透传验证通过（服务端日志含客户端 trace_id）
5. 优雅关闭：在途请求完成，grace 超时后强制关闭且记录日志
6. grep 验证：engine/rpc 之外无 `grpc::` 直接调用（统一走封装）
7. benchmark 输出 p50/p99/QPS 并写入 docs/PERFORMANCE.md
8. Debug / Release 双构建通过，ctest -R Rpc 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在 Combat / Movement / AOI / Buff Tick 内发起同步 RPC（红线扫描 + 代码评审）
- 禁止绕过 GrpcClient 直接使用 grpc::Stub
- 禁止对非幂等调用启用自动重试
- 禁止无限重试或固定间隔重试（必须指数退避 + 抖动 + 上限）
- 禁止每次调用新建 channel
- 禁止在 RPC 路径上吞掉错误返回 bool

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

本机回环 Echo：p50 < 200us、p99 < 1ms；单连接 QPS > 20k；超时控制误差 < 10ms；客户端单次调用额外开销（相对裸 stub）< 5us。

## 23. Deliverables

- engine/rpc/include/mmo/rpc/grpc_client.h
- engine/rpc/include/mmo/rpc/grpc_server.h
- engine/rpc/include/mmo/rpc/status_mapping.h
- engine/rpc/include/mmo/rpc/grpc_channel_pool.h
- engine/rpc/src/*.cpp
- engine/rpc/tests/*
- protocol/proto/service/*.proto
- engine/rpc/docs/INTERFACE.md
- engine/rpc/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-006.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-006.sh
bash scripts/verify/task-006.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 001 005`
2. 交付物存在性检查（6 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Rpc`
5. Benchmark 执行：`bin/rpc_bench --iterations 100000`
6. 性能阈值断言：`bench/rpc.txt` 中 `rpc_p99_us` ≤ `1000`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-006

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(core): RPC Framework（gRPC 统一封装）

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-006.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-006
EOF

# 3) 推送：GFW 屏蔽 ssh.github.com，必须走 22 端口
git push git@github.com:22:shengmingaini/CAMI.git main
```

提交规范：

- 类型：`feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build` / `chore`（本任务建议 `feat`）
- **一个 TASK 一次独立提交**，禁止把多个 TASK 合并提交
- 正文必须包含实测数字，禁止「性能良好 / 已优化」这类无法验证的描述
- 未通过验收脚本禁止提交，禁止 `--no-verify` 绕过

## 26. Codex Execution Rules

1. 读规范：先读 `PROJECT_REQUIREMENTS.md` 与本任务涉及章节，架构冻结，不得自行推翻。
2. 读任务：完整读完本文件全部章节再动手，禁止只看 Objective 就开始写。
3. 查依赖：确认 TASK-001, TASK-005 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-006.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-001` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-005` · `protocol`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
