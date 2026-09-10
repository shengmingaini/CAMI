---
TASK-ID: TASK-005
NAME: Protocol Schema（Protobuf + FlatBuffers）
PHASE: Phase 1 · 统一通信
MODULE: protocol
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-001
---

# TASK-005 · Protocol Schema（Protobuf + FlatBuffers）

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-005` |
| NAME | Protocol Schema（Protobuf + FlatBuffers） |
| PHASE | Phase 1 · 统一通信 |
| MODULE | `protocol` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-001` |

---

## 1. Objective

定义全项目唯一的消息契约：MessageEnvelope / Command / Query / Event 的 Protobuf 定义，以及高频游戏数据的 FlatBuffers 定义。C++ 侧可 Encode / Decode / Validate / Version Check。这是后续所有跨进程与客户端通信的唯一真相源。

## 2. Dependencies

### 2.1 前置任务

- `TASK-001` · Core Error / Result 系统

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 001`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`protocol`

## 4. State Owner（状态归属）

协议 Schema 是契约资产，不持有运行时状态。Codec 实例无状态、可并发复用。MessageEnvelope 一旦构造即为只读：message_id / message_type / version / source / timestamp_ms / trace_id / request_id / payload 八字段（经济类加 transaction_id + idempotency_key），禁止在传输链路中途改写任一字段。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

PROJECT_REQUIREMENTS.md 第 36 节 Message Envelope；TASK-001 ErrorCode（协议错误映射）

## 6. Output

protocol/proto/*.proto、protocol/flatbuffers/*.fbs、生成代码接入 CMake、编解码与版本校验库 + 测试

## 7. Public Interface

```cpp
namespace mmo::protocol {
enum class MessageType : uint8_t { Command = 1, Query = 2, Event = 3, Response = 4, Heartbeat = 5 };
struct EnvelopeView {                      // 解码后的零拷贝视图
  MessageId   message_id; MessageType type; uint32_t version;
  std::string_view source; int64_t timestamp_ms;
  TraceID trace_id; RequestID request_id;
  std::string_view payload;
};
class ICodec { public:
  virtual ~ICodec() = default;
  virtual Result<Buffer> Encode(const EnvelopeView&) = 0;
  virtual Result<EnvelopeView> Decode(std::string_view bytes) = 0;
};
class ProtobufCodec final : public ICodec {};   // 低频：管理面 / 数据面
class FlatbufCodec final : public ICodec {};    // 高频：移动 / AOI / 战斗
class EnvelopeValidator { public:
  static Result<void> Validate(const EnvelopeView&, uint32_t expected_version);
};
}
```

## 8. Data Model

**MessageEnvelope（proto）**

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| message_id | fixed64 | Y | 全链路唯一（= Uuid V4 前 8 字节或雪花） |
| message_type | enum | Y | Command/Query/Event/Response/Heartbeat |
| version | uint32 | Y | 协议版本，用于兼容校验 |
| source | string | Y | 产生者标识 `gateway-01` / `gamenode-07` |
| timestamp_ms | int64 | Y | 产生时刻（墙钟，仅用于审计） |
| trace_id | fixed64 | Y | 全链路追踪 |
| request_id | fixed64 | Y | 单次请求唯一 |
| payload | bytes | Y | 具体 Command/Query/Event 序列化结果 |
| transaction_id | fixed64 | N | 仅经济类消息 |
| idempotency_key | string | N | 仅经济类消息 |

**版本规则**：`version` 为主版本号，破坏性变更才 +1；解码端遇到不支持版本返回 `VERSION_CONFLICT`，禁止静默降级。

## 9. Thread Model

Codec 无状态、线程安全，可在任意线程使用。生成代码只读，禁止手改 `*.pb.h` / `*_generated.h`。

## 10. Hot Path

**YES** （FlatBuffers 编解码位于网络与复制热路径）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- protocol/proto/
- protocol/flatbuffers/
- protocol/include/mmo/protocol/
- protocol/src/
- protocol/tests/
- protocol/docs/
- protocol/CMakeLists.txt

## 15. Implementation Steps

1. 定义 protocol/proto/envelope.proto：MessageEnvelope + MessageType 枚举 + ErrorResponse
2. 定义 protocol/proto/common.proto：Vec3 / EntityId / PlayerId / SceneId 等公共标量包装（禁止各模块自己定义坐标结构）
3. 定义 protocol/proto/command.proto：CommandHeader + `oneof` 占位（MovePlayer / CastSkill 等先放最小集合，后续任务增量扩展）
4. 定义 protocol/proto/query.proto 与 event.proto：QueryHeader / EventHeader 同样用 oneof 预留
5. 定义 protocol/flatbuffers/movement.fbs：位置/朝向/速度，**字段顺序写入后禁止调整**（FlatBuffers 兼容性要求）
6. 定义 protocol/flatbuffers/aoi.fbs：EntityEnter / EntityLeave / PositionUpdate 批量帧
7. 定义 protocol/flatbuffers/combat.fbs：DamageEvent / HealEvent / SkillCast 的最小字段集
8. 在 protocol/CMakeLists.txt 中接入 protoc 与 flatc 代码生成，产物输出到 build/generated，**不入库**
9. 实现 codec/protobuf_codec.h/.cpp 与 codec/flatbuf_codec.h/.cpp，统一实现 ICodec
10. 实现 envelope_validator：必填字段校验、版本校验、payload 非空校验，失败返回明确 ErrorCode
11. 写测试：往返编解码一致性（Protobuf / FlatBuffers 各 1000 次随机数据）；版本不匹配返回 VERSION_CONFLICT；缺失必填字段返回 INVALID_ARGUMENT；FlatBuffers 零拷贝验证（decode 不做内存拷贝，用 buffer 指针断言）
12. 写 benchmark：1e6 次小消息编解码耗时与分配次数
13. 写 docs/VERSIONING.md：协议演进规则（只增字段、禁改类型、禁重排 FBS 字段）

## 16. Unit Test

Envelope 必填字段齐全性校验；Protobuf 编解码往返；FlatBuffers 编解码往返与零拷贝断言；版本校验（相同/更高/更低三种情况）；oneof payload 正确识别；错误码映射正确

## 17. Integration Test

模拟跨进程：进程 A（测试内两个 Codec 实例模拟）编码 → 字节流经文件落盘 → 进程 B 读取并解码，结果一致；Protobuf 与 FlatBuffers 两条链路各自跑通；经济类消息携带 transaction_id + idempotency_key 透传不丢

## 18. Benchmark

bin/protocol_bench：`pb_encode_ns=` / `pb_decode_ns=` / `fbs_encode_ns=` / `fbs_decode_ns=` / `alloc_per_op=`（FlatBuffers 解码目标 0 拷贝）

## 19. Failure Test

payload 空字节：返回 INVALID_ARGUMENT；截断的字节流：解码返回错误而非崩溃（fuzz 1000 次随机截断，用 libFuzzer 或简易随机变异）；version 高于支持版本：VERSION_CONFLICT；恶意超大 payload（>16MB）：被 MaxPayloadBytes 拦截

## 20. Acceptance Criteria

1. MessageEnvelope 九项字段全部定义且 C++ 可 Encode/Decode（单测断言每个字段往返一致）
2. 版本不匹配时返回 `VERSION_CONFLICT`，**不静默降级**
3. FlatBuffers 解码路径分配次数 = 0（零拷贝，benchmark 实测）
4. 1000 次随机截断 fuzz 无崩溃、无越界（ASan 下跑）
5. 生成代码不入库（`git status` 不出现 *.pb.h / *_generated.h）
6. protocol/docs/VERSIONING.md 存在且写明演进规则
7. Debug / Release 双构建通过，ctest -R Protocol 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止任何模块定义第二套消息头（grep 全仓，只认 protocol 模块的 Envelope）
- 禁止手改 protoc / flatc 生成的代码
- 禁止调整已发布 .fbs 文件的字段顺序
- 禁止在协议中传递明文凭据
- 禁止 payload 无上限（必须设 MaxPayloadBytes，默认 1MB，战斗帧另设）
- 禁止协议版本不匹配时静默降级或跳过校验

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Protobuf 小消息（<256B）编解码 < 2us；FlatBuffers 编解码 < 300ns；FlatBuffers 解码分配次数 = 0；单条消息内存占用 < 协议大小的 1.5 倍。

## 23. Deliverables

- protocol/proto/envelope.proto
- protocol/proto/common.proto
- protocol/proto/command.proto
- protocol/proto/query.proto
- protocol/proto/event.proto
- protocol/flatbuffers/movement.fbs
- protocol/flatbuffers/aoi.fbs
- protocol/flatbuffers/combat.fbs
- protocol/include/mmo/protocol/codec/*.h
- protocol/src/codec/*.cpp
- protocol/tests/*
- protocol/docs/VERSIONING.md
- protocol/docs/INTERFACE.md
- protocol/CMakeLists.txt

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-005.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-005.sh
bash scripts/verify/task-005.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 001`
2. 交付物存在性检查（11 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Protocol`
5. Benchmark 执行：`bin/protocol_bench --iterations 1000000`
6. 性能阈值断言：`bench/protocol.txt` 中 `fbs_decode_ns` ≤ `300`
7. 性能阈值断言：`bench/protocol.txt` 中 `fbs_decode_allocs` ≤ `0`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-005

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(protocol): Protocol Schema（Protobuf + FlatBuffers）

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-005.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-005
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
3. 查依赖：确认 TASK-001 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-005.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-001` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
