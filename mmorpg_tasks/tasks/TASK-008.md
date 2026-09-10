---
TASK-ID: TASK-008
NAME: Network Transport（TCP 第一版）
PHASE: Phase 2 · Gateway
MODULE: engine/net
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-004, TASK-005
---

# TASK-008 · Network Transport（TCP 第一版）

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-008` |
| NAME | Network Transport（TCP 第一版） |
| PHASE | Phase 2 · Gateway |
| MODULE | `engine/net` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-004`, `TASK-005` |

---

## 1. Objective

实现 INetworkTransport 抽象与 TCP 第一版实现：Connection / Packet / Buffer / Encode / Decode / Send / Receive，并通过 1K / 5K / 10K 连接基准测试。未来可扩展 UDP / QUIC 而不改动上层。

## 2. Dependencies

### 2.1 前置任务

- `TASK-004` · Core Memory / Thread / Scheduler
- `TASK-005` · Protocol Schema（Protobuf + FlatBuffers）

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 004 005`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`engine/net`

## 4. State Owner（状态归属）

Connection 对象由 Gateway 的网络 IO 线程独占写入（socket 状态、收发缓冲）。业务线程只能提交发送请求，禁止直接触碰 socket。连接表的增删由 Acceptor / Reactor 线程独占，禁止跨线程操作。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-004 MPMC 队列与线程；TASK-005 协议编解码

## 6. Output

engine/net 模块（transport 抽象 + TCP 实现 + 缓冲管理）+ 连接数基准 + 长稳测试

## 7. Public Interface

```cpp
namespace mmo::net {
class IConnection { public: virtual ~IConnection() = default;
  virtual ConnectionId Id() const noexcept = 0;
  virtual Result<void> Send(std::span<const uint8_t>) = 0;      // 零拷贝语义：写入发送缓冲
  virtual Result<void> Close(CloseReason) noexcept = 0;
  virtual std::string_view RemoteAddr() const noexcept = 0;
  virtual ConnectionStats Stats() const noexcept = 0; };
struct TransportEvent {                                          // 事件驱动，禁止回调重入
  enum class Kind { Connected, Disconnected, Received, SendDrained, Error } kind;
  ConnectionId conn_id; core::Error error; std::span<const uint8_t> data; };
class INetworkTransport { public: virtual ~INetworkTransport() = default;
  virtual Result<void> Listen(std::string_view addr, uint16_t port) = 0;
  virtual Result<void> Stop() noexcept = 0;
  virtual Result<void> Poll(DurationMs timeout, std::vector<TransportEvent>& out) = 0;  // 由宿主线程驱动
  virtual size_t ConnectionCount() const noexcept = 0;
  virtual TransportStats Stats() const noexcept = 0; };
std::unique_ptr<INetworkTransport> CreateTcpTransport(TcpConfig);   // 第一版
// TcpConfig: io_threads=2, max_connections=50000, recv_buf=64KB, send_buf=256KB,
//            tcp_nodelay=true, keepalive_idle=30s, backlog=1024
}
```

## 8. Data Model

| 结构 | 说明 |
|---|---|
| ConnectionId | uint64，单调递增，复用 SlotMap 索引避免 ABA |
| Buffer | 环形缓冲，读写指针分离；发送缓冲满时返回 BUSY 而非阻塞 |
| Packet | 4 字节长度前缀 + payload（大端），长度上限由 MaxPayloadBytes 约束 |
| TransportStats | conn_count / bytes_in / bytes_out / packets_in / packets_out / send_queue_depth / error_count |

## 9. Thread Model

IO 线程（NetworkThread）只做 Poll + 数据搬运，**不做业务解包**。解包与处理投递到 Worker/Simulation 线程。每个连接的状态由所属 IO 线程独占，跨线程操作通过事件队列。

## 10. Hot Path

**YES** （收发位于网络热路径）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**YES**

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- engine/net/include/mmo/net/
- engine/net/src/
- engine/net/tests/
- engine/net/benchmark/
- engine/net/docs/

## 15. Implementation Steps

1. 实现 buffer.h：环形缓冲（读/写双指针，支持 scatter/gather iovec）
2. 实现 connection.h/.cpp：ConnectionId 分配（SlotMap）、发送队列、流量统计、优雅关闭（先发完发送缓冲再 FIN）
3. 实现 tcp_transport.h/.cpp：Windows 用 IOCP / Linux 用 epoll（先做跨平台抽象层 `poller.h`，第一版 Windows 优先，因为本地验证在 Windows）
4. 实现 Poll 事件模型：单次 Poll 收集批量事件，禁止回调重入业务代码
5. 实现粘包/半包处理：长度前缀解析，半包保留在连接缓冲，禁止丢弃或假设一帧一包
6. 实现背压：单连接发送缓冲上限 256KB，超限返回 BUSY 并由上层决定（限速/断开），禁止无限堆积
7. 实现连接数限制与拒绝策略：超过 max_connections 直接拒绝新连接并计数
8. 实现 keepalive 与空闲超时清理（心跳由 TASK-009 负责，本任务只做传输层 keepalive）
9. 写测试：单连接 echo 往返；半包/粘包（一次发 0.5 包、一次发 3 包）；大包（1MB）分片收发；连接关闭后资源回收；错误地址/端口占用返回明确 Error
10. 写 benchmark 工具 tools/netbench/：起 N 个客户端连接，测量 `conn_established_ms` / `pps` / `mbps` / `cpu_percent` / `mem_mb` / `fd_count`
11. 跑 1K / 5K / 10K 连接基准并把结果写入 docs/PERFORMANCE.md

## 16. Unit Test

环形缓冲读写/回绕/满/空；ConnectionId 唯一与复用安全；长度前缀解析（半包、粘包、超大包拒绝）；发送缓冲背压与 BUSY；SlotMap 回收后不误发旧连接

## 17. Integration Test

本机起服务端 + 1000 个客户端连接，每个客户端每秒发 20 条 64B 消息，跑 60 秒：无断连、无消息错乱、服务端收到的消息总数 = 期望值；中途 kill 100 个客户端，服务端正确感知断连并回收资源（连接数回落）

## 18. Benchmark

bin/net_bench + tools/netbench：1K / 5K / 10K 连接各跑 60 秒，输出 `conn_count` / `established_ms` / `pps` / `mbps` / `cpu_percent` / `rss_mb` / `per_conn_mem_kb`

## 19. Failure Test

端口被占用：Listen 返回明确错误而非崩溃；客户端异常断连（RST）：服务端收到 Disconnected 事件并回收；发送缓冲持续打满：返回 BUSY 并记录指标，不 OOM；半开连接（只连不发）：keepalive + 空闲超时清理生效；突发 10000 连接同时建立：不崩溃、建立耗时可测、失败连接被正确计数

## 20. Acceptance Criteria

1. 1K / 5K / 10K 连接基准全部跑完，结果数据写入 docs/PERFORMANCE.md（含 CPU / 内存 / PPS）
2. 半包与粘包处理正确（单测 + 集成测试双重覆盖）
3. 单连接发送缓冲背压生效，不出现无界内存增长（长稳 10 分钟 RSS 平稳）
4. 异常断连可被感知并回收资源，连接计数准确回落
5. 传输层**不含任何游戏逻辑**（grep：net/ 目录无 combat/scene/quest 字样）
6. 上层只依赖 INetworkTransport 抽象（grep：除 factory 外无 TcpTransport 直接引用）
7. Debug / Release 双构建通过，ctest -R Net 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在 IO 线程做业务解包或数据库访问
- 禁止回调重入业务代码（必须事件队列）
- 禁止发送缓冲无界增长
- 禁止假设「一次 recv = 一个完整包」
- 禁止在传输层引入游戏语义（玩家、场景、战斗）
- 禁止上层直接依赖 TCP 实现（只依赖抽象）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

10K 空闲连接：RSS < 200MB、per-conn 内存 < 20KB、CPU 空闲 < 2%；1K 活跃连接 20 msg/s：p99 投递延迟 < 1ms；单核 PPS > 100k（64B 消息）；连接建立 10K 总耗时 < 3s。

## 23. Deliverables

- engine/net/include/mmo/net/transport.h
- engine/net/include/mmo/net/connection.h
- engine/net/include/mmo/net/buffer.h
- engine/net/src/*.cpp
- engine/net/tests/*
- engine/net/benchmark/*
- tools/netbench/*
- engine/net/docs/INTERFACE.md
- engine/net/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-008.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-008.sh
bash scripts/verify/task-008.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 004 005`
2. 交付物存在性检查（5 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Net`
5. Benchmark 执行：`bin/net_bench --connections 10000 --duration 60 --idle`
6. 性能阈值断言：`bench/net_10k_idle.txt` 中 `per_conn_mem_kb` ≤ `20`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-008

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(core): Network Transport（TCP 第一版）

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-008.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-008
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
3. 查依赖：确认 TASK-004, TASK-005 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-008.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-004` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
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
