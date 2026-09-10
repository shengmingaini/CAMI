---
TASK-ID: TASK-026
NAME: DataService Interface
PHASE: Phase 6 · 数据系统
MODULE: server/dataservice
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DONE-DATE: 2026-09-10
DEPENDENCIES: TASK-005, TASK-006
---

# TASK-026 · DataService Interface

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-026` |
| NAME | DataService Interface |
| PHASE | Phase 6 · 数据系统 |
| MODULE | `server/dataservice` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-005`, `TASK-006` |

---

## 1. Objective

**先不接真实数据库**，定义统一 IRepository / IDataStore / ICache 接口与 Load/Save/Update/Delete/Batch/VersionCheck 语义。后面 Redis / MySQL 都只是这套接口的实现。

## 2. Dependencies

### 2.1 前置任务

- `TASK-005` · Protocol Schema（Protobuf + FlatBuffers）
- `TASK-006` · RPC Framework（gRPC 统一封装）

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 005 006`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/dataservice`

## 4. State Owner（状态归属）

DataService 是持久化数据的唯一 Owner 与唯一访问入口，GameNode 禁止直连 MySQL 或 Redis。数据版本号（Version）由 DataService 独占推进，乐观锁冲突以 DataService 的判定为准。IRepository 接口契约由本任务独占定义。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-005 协议（数据对象序列化）；TASK-006 RPC（GameNode↔DataService 通信）

## 6. Output

dataservice 接口层 + 内存实现（FakeDataStore）+ 版本冲突测试

## 7. Public Interface

```cpp
namespace mmo::data {
using DataKey = std::string;                 // "character:10086" / "inventory:10086"
struct Record { DataKey key; std::string payload;   // 序列化后的 bytes
                uint32_t version{0}; core::SteadyTime updated_at; };
struct VersionCheck { uint32_t expected_version; bool required{true}; };  // 乐观锁
class IDataStore { public: virtual ~IDataStore() = default;   // 权威持久化（MySQL 类）
  virtual core::Result<std::optional<Record>> Load(const DataKey&) = 0;
  virtual core::Result<void> Save(const Record&, VersionCheck = {}) = 0;
  virtual core::Result<void> Delete(const DataKey&, VersionCheck = {}) = 0;
  virtual core::Result<std::vector<Record>> BatchLoad(std::span<const DataKey>) = 0;
  virtual core::Result<void> BatchSave(std::span<const Record>) = 0; };
class ICache { public: virtual ~ICache() = default;           // 缓存（Redis 类）
  virtual core::Result<std::optional<Record>> Get(const DataKey&) = 0;
  virtual core::Result<void> Put(const Record&, DurationMs ttl = {}) = 0;
  virtual core::Result<void> Invalidate(const DataKey&) = 0;
  virtual core::Result<void> InvalidatePrefix(std::string_view) = 0; };
template <typename T> class IRepository { public: virtual ~IRepository() = default;
  virtual core::Result<std::optional<T>> GetById(uint64_t id) = 0;
  virtual core::Result<void> Put(const T&, VersionCheck = {}) = 0;
  virtual core::Result<void> Remove(uint64_t id) = 0; };
class DataService { public:                                   // 组合层：cache-aside
  core::Result<std::optional<Record>> Load(const DataKey&);
  core::Result<void> Save(const Record&, VersionCheck = {});
  core::Result<void> Flush();                                 // 批量落盘
  DataServiceStats Stats() const noexcept; };                 // hit_rate / pending_writes
}
```

## 8. Data Model

**Cache-Aside 读路径**：`Cache.Get → miss → Store.Load → Cache.Put → 返回`
**Write-Behind 写路径**：`写 Cache → 标记 dirty → 异步批量 Flush 到 Store`（本任务只定义，TASK-027/028 实现）

**版本冲突**：`VersionCheck.required=true` 且期望版本 ≠ 实际版本 → 返回 `VERSION_CONFLICT`，由调用方决定重试或放弃。**禁止静默覆盖**。

**数据分类（决定走哪条路径）**

| 类别 | 例子 | 策略 |
|---|---|---|
| Realtime | HP/MP/位置/战斗状态 | 只在 GameNode 内存，**不落库** |
| Normal Persistent | 角色/任务/装备/背包 | 异步写，允许秒级延迟 |
| Strong Consistency | 货币/交易/拍卖/购买/奖励 | 必须走 Ledger + 幂等（TASK-030） |

## 9. Thread Model

DataService 是**独立进程**；GameNode 通过 gRPC 调用（TASK-006）。DataService 内部用 Worker/Persistence 线程池处理请求，禁止在 IO 线程做序列化。GameNode 侧的调用必须异步，禁止在 Tick 内同步等待。

## 10. Hot Path

**NO**


## 11. External IO

**YES**

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**YES**

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**YES**

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/dataservice/include/mmo/data/
- server/dataservice/src/
- server/dataservice/tests/
- server/dataservice/docs/
- protocol/proto/service/data_service.proto

## 15. Implementation Steps

1. 定义 data/idata_store.h、data/icache.h、data/irepository.h：三套接口（纯虚，无实现）
2. 定义 data/record.h：Record / DataKey / VersionCheck 与键命名规范（`<domain>:<id>`）
3. 实现 data/in_memory_store.h：IDataStore 的内存实现（用于测试与无数据库启动）
4. 实现 data/in_memory_cache.h：ICache 的内存实现（有界 LRU + TTL）
5. 实现 data/data_service.h/.cpp：组合层，实现 cache-aside 读与 write-behind 写队列
6. 实现版本检查：Save 带 VersionCheck，冲突返回 VERSION_CONFLICT（含期望值与实际值）
7. 实现批量：BatchLoad / BatchSave，批量失败时返回**每条**的结果（禁止整批吞错）
8. 实现指标：cache_hit_rate / pending_writes / flush_latency / conflict_count
9. 定义 protocol/proto/service/data_service.proto：Load/Save/BatchLoad/BatchSave/Delete 五个 RPC
10. 实现 FakeDataStore 测试替身：可注入延迟、错误、版本冲突（用于测试重试与冲突路径）
11. 写测试：cache-aside 命中/未命中；版本冲突；批量部分失败；TTL 过期；LRU 淘汰；写入队列 flush

## 16. Unit Test

三接口语义与错误码；Record 版本递增；VersionCheck 冲突判定；批量结果逐条返回；LRU 与 TTL；指标统计正确

## 17. Integration Test

起 DataService 进程（内存实现）+ GameNode 测试客户端：1000 次 Load/Save 混合操作，验证 cache 命中率随访问模式变化符合预期、版本冲突可复现、批量操作吞吐达标；Kill DataService 后 GameNode 侧收到明确错误而非崩溃

## 18. Benchmark

bin/data_bench：`load_ns=` / `save_ns=` / `batch_load_ns_per_1k=` / `cache_hit_ns=` / `flush_ns_per_1k=`

## 19. Failure Test

Store 不可用（注入故障）：返回明确错误，缓存仍可服务读（stale 读按配置开关）；版本冲突：返回 VERSION_CONFLICT 且不覆盖；批量中部分失败：成功部分生效，失败部分带索引返回；Cache 不可用：降级直连 Store（降级开关 + 指标）；flush 队列积压：触发背压，返回 BUSY 而非 OOM

## 20. Acceptance Criteria

1. IRepository / IDataStore / ICache 三套接口定义完成，无具体数据库依赖
2. Load / Save / Update / Delete / Batch / VersionCheck 六种操作语义齐全
3. **版本冲突返回 VERSION_CONFLICT，不静默覆盖**（单测断言）
4. 内存实现可跑通全部测试（无需真实数据库即可验收）
5. 批量操作失败时逐条返回结果（禁整批吞错）
6. data_service.proto 定义完成，RPC 签名与接口一致
7. Debug / Release 双构建通过，ctest -R DataService 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在本任务接入真实 Redis / MySQL（只定义接口 + 内存实现）
- 禁止版本冲突时静默覆盖
- 禁止批量操作吞掉部分失败
- 禁止 GameNode 在 Tick 内同步等待 DataService
- 禁止在 DataService 接口中混入业务语义（它只管键值与版本）
- 禁止把实时状态（HP/位置）写进 DataService

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

内存实现：Load < 500ns；Save < 1us；批量 1000 条 Load < 5ms；cache 命中率（热点数据）> 95%。

## 23. Deliverables

- server/dataservice/include/mmo/data/idata_store.h
- server/dataservice/include/mmo/data/icache.h
- server/dataservice/include/mmo/data/irepository.h
- server/dataservice/include/mmo/data/data_service.h
- server/dataservice/src/*.cpp
- server/dataservice/tests/*
- protocol/proto/service/data_service.proto
- server/dataservice/docs/INTERFACE.md
- server/dataservice/docs/README.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-026.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-026.sh
bash scripts/verify/task-026.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 005 006`
2. 交付物存在性检查（2 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R DataService`
5. Benchmark 执行：`bin/data_bench --ops 100000`
6. 性能阈值断言：`bench/data.txt` 中 `load_ns` ≤ `500`
7. 性能阈值断言：`bench/data.txt` 中 `cache_hit_ns` ≤ `300`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-026

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): DataService Interface

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-026.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-026
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
3. 查依赖：确认 TASK-005, TASK-006 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-026.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-005` · `protocol`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-006` · `engine/rpc`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
