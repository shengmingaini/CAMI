# -*- coding: utf-8 -*-
"“”TASK-030 ~ TASK-038：Phase 6 账本 + Phase 7 Lua + Phase 8 客户端 + Phase 9 容灾 + Phase 10 交付“”"

OWNER = "Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证"

TASKS = [
# ------------------------------------------------------------------ 030
dict(
 id="TASK-030", name="Economic Ledger / Idempotency", phase="Phase 6 · 数据系统",
 objective="实现经济账本与幂等性：TransactionID / RequestID / IdempotencyKey / Version / Ledger。**必须证明：重复请求、RPC 重试、断线、GameNode Crash、数据库重试五种场景下都不会重复扣钱、不会重复发奖励、不会复制装备。**",
 deps="TASK-001, TASK-005, TASK-026, TASK-028, TASK-029",
 module="server/gamenode/economy",
 owner=OWNER,
 inp="TASK-029 EconomyCommand（已含 transaction_id 与 idempotency_key）；TASK-026 DataService；TASK-028 MySQL",
 out="ledger 模块 + 幂等表 + 五场景故障测试 + 资金守恒对账报告",
 iface="""```cpp
namespace mmo::game::economy {
struct LedgerEntry {                         // 只追加，永不修改（append-only）
  TransactionId transaction_id; core::RequestID request_id;
  std::string idempotency_key;               // 唯一索引
  PlayerId player; EconomyOp op;
  CurrencyType currency; int64_t delta; int64_t balance_after;
  std::vector<ItemDelta> item_deltas;
  std::string reason; std::string source;
  int64_t timestamp_ms; uint32_t version;
  std::array<uint8_t,32> prev_hash;          // 账本链，防篡改
  std::array<uint8_t,32> hash; };
enum class IdemStatus : uint8_t { Fresh, InFlight, Completed, Failed };
class IIdempotencyStore { public: virtual ~IIdempotencyStore() = default;
  virtual core::Result<IdemStatus> TryBegin(std::string_view key, DurationMs ttl) = 0;
  virtual core::Result<void> Commit(std::string_view key, const EconomyResult&) = 0;
  virtual core::Result<void> Abort(std::string_view key) = 0;
  virtual core::Result<std::optional<EconomyResult>> Lookup(std::string_view key) = 0; };
class Ledger { public:
  core::Result<void> Append(const LedgerEntry&);
  core::Result<void> Flush();                                  // 批量落库，异步
  core::Result<bool> VerifyChain(int64_t from_ts, int64_t to_ts);  // 哈希链校验
  core::Result< std::vector<LedgerEntry> > QueryByPlayer(PlayerId, int64_t from, int64_t to);
  LedgerStats Stats() const noexcept; };     // pending / flushed / duplicates_rejected
}
```""",
 data="""**幂等键生成规则（唯一真相）**

| 场景 | IdempotencyKey 构成 |
|---|---|
| 购买 | `purchase:{player}:{shop_id}:{item_id}:{client_seq}` |
| 任务奖励 | `quest:{player}:{quest_id}` |
| 交易 | `trade:{trade_id}:{side}` |
| 邮件领取 | `mailclaim:{player}:{mail_id}` |
| RPC 重试 | **沿用首次的同一个 key**，禁止重试时重新生成 |

**幂等状态机**：`Fresh → InFlight → Completed | Failed`。`InFlight` 期间同一 key 的并发请求直接返回 BUSY（不重复执行）；`Completed` 的请求返回**首次结果**（deduplicated=true）。

**账本表**（database/migrations/003_ledger.sql）

| 列 | 约束 |
|---|---|
| transaction_id | PRIMARY KEY |
| idempotency_key | **UNIQUE INDEX**（冲突即发现重复） |
| delta / balance_after | BIGINT，非空 |
| prev_hash / hash | BINARY(32)，链式校验 |
| timestamp_ms | BIGINT，分区键（按月） |""",
 thread="幂等判定与余额变更在 SimulationThread 同步完成（必须立即可见）；账本落盘异步投递到 Persistence 线程。落盘失败进入重试队列，**不阻塞内存态**，但必须保证最终落盘（用 in-flight 标记防丢失）。",
 hot="NO", io="YES（异步落盘）", rpc="YES（跨进程数据服务）", persist="YES",
 state="账本为 append-only 权威记录，Owner 是 DataService（MySQL）；幂等表的 InFlight 状态 Owner 是发起方 GameNode，带 TTL 自动过期。",
 files=["server/gamenode/economy/include/mmo/game/economy/ledger/", "server/gamenode/economy/src/ledger/",
        "server/gamenode/economy/tests/", "server/dataservice/src/mysql/", "database/migrations/", "docs/"],
 steps=[
  "定义 ledger_entry.h：LedgerEntry 十六项字段（含 prev_hash/hash 链）",
  "实现 idempotency_store.h/.cpp：四状态机（Fresh/InFlight/Completed/Failed），带 TTL 防悬挂",
  "实现 Redis 版幂等存储（复用 TASK-027 连接池）+ MySQL 版（UNIQUE 索引兜底，双保险）",
  "实现 ledger.h/.cpp：Append（内存环形缓冲 + 异步批量落库）、Flush、VerifyChain、QueryByPlayer",
  "实现哈希链：每条 hash = SHA256(prev_hash + 规范化字段串)，禁止把整条记录序列化后哈希（字段顺序变化会断链）",
  "把 TASK-029 的 EconomySystem 接入幂等：Execute 开头 TryBegin，结尾 Commit，异常路径 Abort",
  "实现重试安全：RPC 重试必须携带**同一个** idempotency_key（在 RpcOptions 中透传，见 TASK-006）",
  "实现 InFlight 过期：TTL 默认 30 秒，过期后按最终状态重放（查账本决定结果）",
  "实现对账工具 tools/audit/economy_audit.py：按玩家汇总账本 delta 与当前余额比对，输出差异报告",
  "写**五场景故障测试**（见下），每个场景必须断言「余额变化次数 == 1」",
  "写资金守恒测试：1 万次随机经济操作后，Σ(所有玩家余额) + Σ(系统回收) == 初始发行量",
 ],
 unit="幂等四状态机全路径；同一 key 并发请求返回 BUSY 且只执行一次；Completed 后重复请求返回首次结果；TTL 过期与重放；哈希链计算与校验；账本 Append 与批量 Flush；对账工具与守恒校验",
 integ="**1 万次混合经济操作**：结束后 Σ账本 delta == Σ余额变化；哈希链完整可校验；对账工具输出零差异；幂等表无悬挂 InFlight",
 bench="bin/ledger_bench：`idem_check_ns=` / `ledger_append_ns=` / `flush_ns_per_1k=` / `dedup_hit_ns=` / `mem_bytes_per_entry=`",
 fail="""**五场景故障测试（本任务的核心，必须全部通过）**

| 场景 | 模拟方式 | 断言 |
|---|---|---|
| 重复请求 | 同一 key 连发 10 次 | 余额只变 1 次，9 次返回 deduplicated=true |
| RPC 重试 | 注入 UNAVAILABLE 让客户端重试 3 次（同 key） | 只扣 1 次钱 |
| 断线重连 | 扣钱成功后立即断线，重连后客户端重发 | 不重复扣 |
| GameNode Crash | 内存中标记 InFlight 后强杀进程，重启 | 从 MySQL 幂等表恢复，不重复执行 |
| 数据库重试 | 落库时注入死锁/超时并重试 | 不产生两条账本记录（UNIQUE + 哈希链双校验） |

额外：装备发放重复场景 —— 同一幂等键不会产出两个 ItemGuid（**不复制装备**）。""",
 accept=[
  "**五个故障场景全部通过**，每个场景断言「余额/物品变化次数 == 1」",
  "幂等表有 UNIQUE 索引兜底（数据库层也挡得住）",
  "账本哈希链可校验，篡改检测生效（单测：改一条记录后 VerifyChain 返回 false）",
  "1 万次操作后资金守恒，对账工具零差异",
  "InFlight 有 TTL，崩进程后能恢复，无悬挂",
  "重复请求返回首次结果而非报错（对客户端友好）",
  "Debug / Release 双构建通过，ctest -R Economy_Ledger 全绿",
 ],
 forbid=[
  "禁止 RPC 重试时重新生成 idempotency_key",
  "禁止幂等状态无 TTL（会永久悬挂）",
  "禁止账本记录被 UPDATE/DELETE（只能 append）",
  "禁止在没有 UNIQUE 索引的情况下依赖应用层去重",
  "禁止在 Tick 内同步等待账本落库",
  "禁止允许负余额或重复发放奖励",
 ],
 perf="幂等检查 < 200ns（Redis 路径 < 200us）；账本 Append < 500ns；批量 Flush 1000 条 < 50ms；单条账本内存 < 256B；重复请求拦截率 100%。",
 deliver=["server/gamenode/economy/include/mmo/game/economy/ledger/ledger.h",
          "server/gamenode/economy/include/mmo/game/economy/ledger/idempotency_store.h",
          "server/gamenode/economy/src/ledger/*.cpp", "server/gamenode/economy/tests/*",
          "database/migrations/003_ledger.sql", "tools/audit/economy_audit.py",
          "server/gamenode/economy/docs/INTERFACE.md", "docs/economy-failure-test-report.md"],
 ctest="Economy_Ledger", both_build=True,
 bench_bins=[("bin/ledger_bench", "--ops 10000")],
 metrics=[("bench/ledger.txt", "idem_check_ns", "le", "200"), ("bench/ledger.txt", "mem_bytes_per_entry", "le", "256")],
 artifacts=["server/gamenode/economy/include/mmo/game/economy/ledger/ledger.h", "database/migrations/003_ledger.sql"],
),
# ------------------------------------------------------------------ 031
dict(
 id="TASK-031", name="Lua Runtime", phase="Phase 7 · Lua 脚本",
 objective="实现 Lua 运行时：LuaVM / ScriptContext / Binding / Sandbox / Memory Limit / Execution Limit。C++ 向 Lua 暴露 Entity / Skill / Quest / Event / Query 能力。",
 deps="TASK-001, TASK-004, TASK-005, TASK-007, TASK-011",
 module="scripting/lua",
 owner=OWNER,
 inp="TASK-011 Entity（绑定对象）；TASK-007 EventBus（脚本订阅事件）；TASK-005 协议；TASK-004 内存池（脚本分配）",
 out="scripting/lua 模块 + 沙箱与限制测试 + 绑定测试",
 iface="""```cpp
namespace mmo::script {
struct LuaLimits { size_t memory_bytes{8*1024*1024};      // 单 VM 内存上限
                   uint32_t max_instructions{10'000'000};  // 单次调用指令上限（debug hook 计数）
                   DurationMs max_exec_time{5};            // 单次调用时间上限
                   uint32_t max_stack_depth{64};           // 防无限递归
                   bool allow_io{false};                   // 沙箱：禁 io/os 库
                   bool allow_loadstring{false}; };
class ScriptContext { public:                              // 每 Scene 一个，禁止全局单例
  static core::Result<std::unique_ptr<ScriptContext>> Create(LuaLimits);
  core::Result<ScriptId> Load(std::string_view name, std::string_view source);
  core::Result<void> Unload(ScriptId);
  template <typename... Args> core::Result<void> Call(ScriptId, std::string_view fn, Args&&...);
  core::Result<void> BindEntityApi(entity::EntityManager&);
  core::Result<void> BindEventApi(core::EventBus&);
  core::Result<void> BindQueryApi(core::QueryBus&);
  size_t MemoryUsed() const noexcept; uint32_t Version() const noexcept; };
enum class ScriptError : uint8_t { Ok, CompileError, RuntimeError, MemoryLimit,
                                   InstructionLimit, Timeout, StackOverflow, SandboxViolation };
core::Error ToCoreError(ScriptError) noexcept;
}
```""",
 data="""**C++ → Lua 绑定面（白名单，禁止暴露整个引擎）**

| API | 方法 | 说明 |
|---|---|---|
| Entity | `entity.get(id)` / `entity.set_hp(id,v)` / `entity.get_pos(id)` | 只暴露只读与受控写 |
| Skill | `skill.cast(caster, skill_id, target)` | 走 SkillSystem 命令，禁止脚本直接改伤害 |
| Quest | `quest.set_progress(player, idx, v)` / `quest.complete(player, id)` | 走 QuestSystem |
| Event | `event.subscribe(name, fn)` / `event.publish(name, table)` | 经 EventBus，异步 |
| Query | `query.ask(name, table)` | 只读取，禁止副作用 |

**沙箱**：移除 `io` / `os` / `loadstring` / `require` / `debug`（除 hook）；只保留 `string` / `table` / `math` 的安全子集。
**禁止**：脚本直接访问 Entity 内部内存指针、直接改 HP 数值、做文件/网络 IO。""",
 thread="Lua VM **每 Scene 一个**，只在所属 Scene 的 SimulationThread 执行，**禁止跨线程共享 VM**。脚本调用为同步，受指令数与时间双重上限约束。",
 hot="YES（技能公式、Buff 公式会逐次调用）", io="NO（沙箱禁 IO）", rpc="NO", persist="NO",
 state="脚本自身状态（全局变量）由 ScriptContext 拥有，归属 Scene 的 SimulationThread；脚本不得持有 C++ 对象指针跨 Tick（用 EntityId 句柄）。",
 files=["scripting/lua/include/mmo/script/", "scripting/lua/src/", "scripting/lua/tests/", "scripting/lua/benchmark/", "scripting/lua/docs/"],
 steps=[
  "引入 Lua（vcpkg lua 或 luajit，锁定版本），接入 CMake，产物不入库",
  "实现 lua_vm.h/.cpp：VM 创建与销毁，绑定自定义 allocator（走 TASK-004 MemoryPool，便于限额与统计）",
  "实现 script_context.h/.cpp：Load/Unload/Call，每 Scene 一个实例",
  "实现内存限制：自定义 allocator 计数，超限抛 MemoryLimit 并回滚到安全点",
  "实现指令与时间上限：用 lua_sethook(L, LUA_MASKCOUNT) 计数 + 单调时钟检查，双保险",
  "实现栈深限制：hook 中检查栈深度，防无限递归导致 C 栈溢出",
  "实现沙箱：移除 io/os/loadstring/require/debug，只保留安全子集（白名单而非黑名单）",
  "实现五项绑定：Entity / Skill / Quest / Event / Query，全部走 C++ 系统接口（禁止直改内部）",
  "实现错误映射：ScriptError → mmo::core::Error（TASK-001），含行号与脚本名",
  "写测试：沙箱逃逸尝试（调 io.open 应失败）、内存超限、指令超限、超时、栈溢出、脚本语法错误、脚本运行时错误（不崩溃宿主）、绑定 API 正确性",
  "写 benchmark：脚本调用开销（目标 < 2us/次）",
 ],
 unit="VM 创建销毁；Load/Unload；五项绑定 API；沙箱禁用项（io/os/loadstring/require 均不可用）；内存/指令/时间/栈深四类限制的触发与恢复；错误映射含行号",
 integ="在 Scene 中挂 ScriptContext，用 Lua 实现一个「受击时触发」的脚本：C++ 发事件 → Lua 订阅 → 调用 skill.cast → C++ 结算伤害 → Lua 再收到结果。断言全链路往返正确、限制生效、脚本错误不影响 Scene Tick",
 bench="bin/lua_bench：`lua_call_ns=` / `lua_load_ms_per_script=` / `mem_overhead_bytes=` / `instruction_check_ns=`",
 fail="脚本死循环（while true）：指令上限触发并返回错误，**不卡死 Tick**；脚本内存超限：分配失败并返回 MemoryLimit，VM 可继续跑其他脚本；脚本调用不存在的 API：返回明确错误而非崩溃；脚本抛 error：捕获并转 ScriptError，宿主继续；脚本试图 io.open / os.execute：沙箱拒绝；脚本持有失效 EntityId：返回 NOT_FOUND 而非野指针",
 accept=[
  "**每 Scene 一个 VM，不跨线程共享**（grep + 并发测试）",
  "沙箱生效：io / os / loadstring / require 全部不可用（单测断言）",
  "四类限制（内存/指令/时间/栈深）全部触发正确，且**不卡死 Tick**",
  "脚本错误被捕获，宿主进程不崩溃，Scene 继续 Tick（单测）",
  "五项绑定（Entity/Skill/Quest/Event/Query）可用且只走系统接口",
  "单次脚本调用 < 2us（benchmark 实测）",
  "Debug / Release 双构建通过，ctest -R Lua 全绿",
 ],
 forbid=[
  "禁止全局单例 LuaVM（必须每 Scene 一个）",
  "禁止跨线程共享 VM",
  "禁止脚本直接访问 C++ 对象裸指针（用 EntityId 句柄）",
  "禁止脚本做文件 / 网络 / 数据库 IO",
  "禁止脚本直接改 HP / 伤害数值（必须走系统接口）",
  "禁止无指令/时间上限的脚本调用（会卡死 Tick）",
  "禁止用黑名单做沙箱（必须白名单）",
 ],
 perf="单次脚本调用 < 2us；单 VM 常驻内存开销 < 4MB；指令计数开销 < 10%；脚本加载（100 个脚本）< 100ms。",
 deliver=["scripting/lua/include/mmo/script/script_context.h", "scripting/lua/include/mmo/script/lua_vm.h",
          "scripting/lua/src/*.cpp", "scripting/lua/tests/*", "scripting/lua/benchmark/*",
          "scripting/lua/docs/INTERFACE.md", "scripting/lua/docs/SANDBOX.md"],
 ctest="Lua", both_build=True,
 bench_bins=[("bin/lua_bench", "--calls 1000000")],
 metrics=[("bench/lua.txt", "lua_call_ns", "le", "2000")],
 artifacts=["scripting/lua/include/mmo/script/script_context.h", "scripting/lua/docs/SANDBOX.md"],
),
# ------------------------------------------------------------------ 032
dict(
 id="TASK-032", name="Lua Hot Reload", phase="Phase 7 · Lua 脚本",
 objective="实现 Lua 热更新：Load → Compile → Validate → Activate → Rollback → Version。**只能在 Tick Safe Point 切换，禁止在 Tick 执行中间替换脚本。**",
 deps="TASK-003, TASK-013, TASK-031",
 module="scripting/lua",
 owner=OWNER,
 inp="TASK-031 ScriptContext；TASK-013 SimulationScheduler（Tick 边界安全点）；TASK-003 版本号",
 out="热更模块 + 安全点切换测试 + 回滚测试 + 版本记录",
 iface="""```cpp
namespace mmo::script {
enum class ReloadState : uint8_t { Idle, Compiling, Validating, PendingActivate, Activated, RolledBack, Failed };
struct ScriptVersion { ScriptId id; uint32_t version; std::string checksum;
                       int64_t activated_at_ms; std::string activated_by; };
class HotReloader { public:
  struct Config { bool validate_before_activate{true}; uint32_t max_rollback_versions{5};
                  DurationMs validate_budget{50}; };
  // 阶段 1~3：可在任意线程（通常是运维/控制线程）
  core::Result<ReloadTicket> Prepare(std::string_view name, std::string_view source);
  core::Result<ValidationReport> Validate(ReloadTicket);     // 语法 + 沙箱 + 冒烟执行
  // 阶段 4：**只能在 Tick Safe Point 调用**，由 SimulationThread 执行
  core::Result<void> Activate(ReloadTicket, core::TraceID);
  core::Result<void> Rollback(std::string_view name, core::TraceID);
  const ScriptVersion* CurrentVersion(std::string_view name) const noexcept;
  std::vector<ScriptVersion> History(std::string_view name) const; };
}
```""",
 data="""**热更流水线（六阶段，顺序不可变）**

```
1 Load      读取新源码（任意线程）
2 Compile   lua_load 编译为字节码（任意线程）
3 Validate  语法检查 + 沙箱静态检查 + 冒烟调用（任意线程）
   ─── 以上失败 = 旧版本完全不受影响 ───
4 Activate  【Tick Safe Point】原子替换 ScriptContext 中的函数引用
5 Verify    下一 Tick 冒烟验证，异常则自动 Rollback
6 Commit    记录 ScriptVersion + ConfigVersion 到审计日志
```

**版本记录**：每次成功激活必须记录 `ScriptVersion`（脚本名/版本号/checksum/时间/操作者）与 `ConfigVersion`，写入 `docs/script-versions.md` 与审计日志。
**回滚**：保留最近 5 个版本，Rollback 同样在 Tick Safe Point 执行。""",
 thread="Compile / Validate 在 Worker 线程；**Activate 与 Rollback 只能在 SimulationThread 的 Tick Safe Point**（即 Tick 八阶段全部完成之后、下一 Tick Input 之前）。禁止在 Combat/Buff 阶段中间替换。",
 hot="NO（Activate 位于 Tick 边界，极短）", io="YES（读脚本文件）", rpc="NO", persist="YES（版本审计）",
 state="当前生效脚本版本由 ScriptContext 拥有（SimulationThread）；待激活的 ReloadTicket 由 HotReloader 拥有（Worker 线程），激活瞬间原子交接。",
 files=["scripting/lua/include/mmo/script/hot_reload/", "scripting/lua/src/hot_reload/", "scripting/lua/tests/", "docs/", "tools/scriptctl/"],
 steps=[
  "实现 hot_reloader.h/.cpp：六阶段流水线状态机（Idle→Compiling→Validating→PendingActivate→Activated→RolledBack/Failed）",
  "实现 ReloadTicket：承载编译产物与校验报告，未 Activate 前对运行中的 VM **零影响**",
  "实现 Validate：语法检查 + 禁用 API 静态扫描 + 冒烟执行（在隔离的临时 VM 中跑，不动生产 VM）",
  "实现 Tick Safe Point 接入：向 TASK-013 SimulationScheduler 注册 Tick 边界钩子，Activate 只能在此钩子内执行",
  "实现原子替换：替换函数引用（指针赋值），**不重建 VM**（重建会丢失脚本全局状态）",
  "实现自动回滚：激活后下一 Tick 冒烟验证失败 → 自动 Rollback 并告警",
  "实现版本历史：保留最近 5 个版本（源码 checksum + 字节码），支持按版本号回滚",
  "实现审计：每次激活写日志（脚本名/版本/checksum/时间/操作者）+ 更新 docs/script-versions.md",
  "实现 CLI 工具 tools/scriptctl/：reload / validate / rollback / history / status 五个子命令",
  "写测试：编译失败不影响旧版；校验失败不影响旧版；Activate 在安全点执行（并发注入验证无中间态）；回滚正确；版本历史上限；非安全点调用 Activate 被拒绝",
 ],
 unit="六阶段状态机全路径；编译/校验失败时旧版零影响（断言旧版仍可调用）；Activate 在非安全点返回错误；原子替换后新旧函数引用正确；回滚恢复旧版本；版本历史截断到 5 条；checksum 计算",
 integ="**运行中热更**：Scene 以 20Hz 持续跑战斗（脚本在伤害公式中生效），运维线程发起热更 → 断言切换发生在 Tick 边界（用 Tick 号记录验证切换前后脚本版本不出现在同一 Tick 内）、切换期间无 Tick 超时、切换后新逻辑生效且状态连续（脚本全局变量保留）",
 bench="bin/hotreload_bench：`prepare_ms=` / `validate_ms=` / `activate_us=`（安全点停顿，必须 < 100us）/ `rollback_us=`",
 fail="热更脚本有语法错误：Validate 拦截，旧版本继续服务（**不中断线上**）；热更脚本运行时崩溃：自动 Rollback 并告警，Scene 继续跑；Activate 时正在执行该脚本：等待到安全点（不打断当前调用）；磁盘脚本文件被删：Prepare 返回 NOT_FOUND；连续 10 次热更：版本历史正确截断，内存不泄漏",
 accept=[
  "**Activate 只能在 Tick Safe Point 执行**（非安全点调用返回错误，单测断言）",
  "编译失败 / 校验失败时**线上旧版本完全不受影响**（集成测试断言）",
  "单次 Activate 停顿 < 100us（不产生 Tick 尖峰，benchmark 实测）",
  "自动回滚生效（注入会崩的脚本，验证自动恢复旧版）",
  "版本历史保留最近 5 个版本，可回滚",
  "每次激活有审计日志与 docs/script-versions.md 记录",
  "Debug / Release 双构建通过，ctest -R HotReload 全绿",
 ],
 forbid=[
  "禁止在 Tick 执行中间替换脚本（必须 Tick Safe Point）",
  "禁止热更时重建 VM（会丢失脚本全局状态）",
  "禁止未校验直接激活",
  "禁止热更失败时让线上处于半新半旧状态",
  "禁止无版本记录的热更",
  "禁止无限保留历史版本（内存泄漏）",
  "禁止热更造成 Tick 尖峰（停顿必须 < 100us）",
 ],
 perf="Prepare（编译）< 10ms/脚本；Validate < 20ms/脚本；**Activate 停顿 < 100us**；Rollback < 100us；热更期间 Tick P99 无明显恶化（< 10%）。",
 deliver=["scripting/lua/include/mmo/script/hot_reload/hot_reloader.h",
          "scripting/lua/src/hot_reload/*.cpp", "scripting/lua/tests/*",
          "tools/scriptctl/*", "docs/script-versions.md", "scripting/lua/docs/HOTRELOAD.md"],
 ctest="HotReload", both_build=True,
 bench_bins=[("bin/hotreload_bench", "--scripts 100 --reloads 10")],
 metrics=[("bench/hotreload.txt", "activate_us", "le", "100")],
 artifacts=["scripting/lua/include/mmo/script/hot_reload/hot_reloader.h", "docs/script-versions.md"],
),
# ------------------------------------------------------------------ 033
dict(
 id="TASK-033", name="Gameplay Script", phase="Phase 7 · Lua 脚本",
 objective="用 Lua 实现第一版玩法脚本：Quest Script / Skill Formula / NPC AI / Boss Phase / Event Script。**这是验证「C++ Framework + Lua Rule」是否真正工作的关键任务。**",
 deps="TASK-018, TASK-019, TASK-021, TASK-031, TASK-032",
 module="scripting/gameplay",
 owner=OWNER,
 inp="TASK-031 运行时与绑定；TASK-032 热更；TASK-018/019/021 各 C++ 系统接口",
 out="scripting/gameplay 脚本集 + C++/Lua 分工验证 + 热更实战演练",
 iface="""```lua
-- 契约：所有玩法脚本遵循统一入口（C++ 只认这四个函数）
function on_init(ctx)    end   -- 脚本加载
function on_event(ctx, name, payload) end   -- 事件回调
function on_tick(ctx, dt) end              -- 可选周期（低频，禁止高频）
function on_reload(ctx, old_version) end   -- 热更后的状态迁移
```

**C++ / Lua 分工红线**

| 能力 | 归属 | 理由 |
|---|---|---|
| Simulation / Entity / Memory / Scheduler / Network / AOI / 核心战斗框架 | **C++** | 性能与确定性 |
| Quest 规则 / Skill 公式 / Buff 公式 / NPC AI / Boss 阶段 / 活动脚本 | **Lua** | 迭代频率高 |
| 位置积分、伤害数值结算、AOI 计算 | **C++** | 禁止下放到 Lua |""",
 data="""**第一版脚本清单（至少 5 类 × 各 3 个）**

| 类别 | 脚本 | 说明 |
|---|---|---|
| Quest Script | `quest/kill_10_wolves.lua` 等 | 监听 MonsterKilled 更新进度 |
| Skill Formula | `skill/fireball.lua` 等 | 计算 base + coeff × AP，返回给 C++ 结算 |
| NPC AI | `ai/aggressive_guard.lua` 等 | 状态转移钩子（Idle/Patrol/Chase 决策） |
| Boss Phase | `boss/dragon_phase.lua` 等 | 按 HP 百分比切阶段，切阶段时召唤/换技能 |
| Event Script | `event/double_exp.lua` 等 | 限时活动：经验翻倍，活动结束自动失效 |

**配置化**：脚本路径与绑定关系写在 `config/gameplay/scripts.json`，禁止硬编码到 C++。""",
 thread="脚本在所属 Scene 的 SimulationThread 执行（同步调用）。周期脚本（on_tick）默认 1Hz，禁止高频调用；AI 决策复用 TASK-018 的 5Hz 节流。",
 hot="YES（技能公式在战斗热路径被调用）", io="NO", rpc="NO", persist="NO",
 state="脚本状态归属 ScriptContext（每 Scene）；业务状态（任务进度、Boss 阶段）归属对应 C++ 系统，脚本只能通过接口读写。",
 files=["scripting/gameplay/quest/", "scripting/gameplay/skill/", "scripting/gameplay/ai/",
        "scripting/gameplay/boss/", "scripting/gameplay/event/", "scripting/gameplay/tests/",
        "config/gameplay/scripts.json", "docs/"],
 steps=[
  "定义脚本契约：四入口函数 + ctx 结构（Entity/Skill/Quest/Event/Query API）",
  "实现 C++ 侧脚本加载器：按 config/gameplay/scripts.json 绑定「系统钩子 ↔ 脚本」",
  "编写 3 个 Quest Script：监听事件更新进度、多目标、限时任务",
  "编写 3 个 Skill Formula：返回伤害/治疗公式结果（C++ 仍负责最终结算与随机）",
  "编写 3 个 NPC AI 脚本：巡逻/警戒/逃跑三种行为钩子（复用 C++ 状态机，Lua 只做决策）",
  "编写 3 个 Boss Phase 脚本：按 HP 阈值切阶段、切换技能组、召唤小怪、阶段广播",
  "编写 3 个 Event Script：限时双倍经验、世界事件、节日活动（含自动失效）",
  "实现脚本级单元测试框架（Lua 侧）：可 mock ctx 并断言行为",
  "实现热更实战：用 TASK-032 的 scriptctl 热更一个技能公式，验证线上即时生效且可回滚",
  "写**分工验证测试**：脚本试图改 HP 数值 / 做 IO → 被拦截（证明红线有效）",
 ],
 unit="15 个脚本各自的 Lua 侧单测（mock ctx）；脚本加载与绑定；四入口函数齐全；脚本违反红线被拦截；配置驱动加载",
 integ="**端到端玩法验证**：玩家接任务 → 杀怪（C++ 发事件）→ Lua 任务脚本更新进度 → 完成任务 → Lua 奖励脚本发奖 → 玩家用 Lua 公式技能打 Boss → Boss 血量到 70% → Lua 阶段脚本切阶段 → 全员收到广播。全链路跑通，且每一步的状态归属正确",
 bench="bin/gameplay_script_bench：`skill_formula_ns=` / `quest_script_ns=` / `boss_phase_check_ns=` / `script_total_cpu_percent=`",
 fail="脚本语法错误：加载失败并回退到 C++ 默认行为（禁止整个系统崩）；脚本超时：指令上限拦截，本次调用作废；脚本返回非法值（NaN/负伤害）：C++ 侧校验并钳制；热更后脚本状态丢失：on_reload 正确迁移；脚本引用的 Quest/Boss 不存在：加载期校验报错",
 accept=[
  "五类脚本（Quest / Skill Formula / NPC AI / Boss Phase / Event）各至少 3 个，共 ≥ 15 个",
  "**端到端玩法链路跑通**（集成测试，见上）",
  "脚本路径配置化，C++ 无硬编码脚本名",
  "脚本违反红线（改 HP / 做 IO）被拦截（单测）",
  "热更一个技能公式，线上即时生效且可回滚（实战演练）",
  "脚本总 CPU 占比 < 10%（benchmark 实测，禁止把所有计算塞进 Lua）",
  "Debug / Release 双构建通过，ctest -R GameplayScript 全绿",
 ],
 forbid=[
  "禁止把位置积分 / AOI / 伤害最终结算下放到 Lua",
  "禁止脚本直接改 HP、伤害数值等实时状态（必须走系统接口）",
  "禁止脚本做文件 / 网络 / 数据库 IO",
  "禁止高频（> 1Hz）调用周期脚本",
  "禁止硬编码脚本路径",
  "禁止脚本错误导致整个 Scene 崩溃",
  "禁止脚本总 CPU 占比超过 10%",
 ],
 perf="单次技能公式调用 < 3us；单次任务脚本处理 < 2us；Boss 阶段检查 < 1us；脚本总 CPU 占比 < 10%；15 个脚本加载 < 50ms。",
 deliver=["scripting/gameplay/quest/*.lua", "scripting/gameplay/skill/*.lua", "scripting/gameplay/ai/*.lua",
          "scripting/gameplay/boss/*.lua", "scripting/gameplay/event/*.lua", "scripting/gameplay/tests/*",
          "config/gameplay/scripts.json", "scripting/gameplay/docs/README.md", "docs/gameplay-script-report.md"],
 ctest="GameplayScript", both_build=True,
 bench_bins=[("bin/gameplay_script_bench", "--iterations 100000")],
 metrics=[("bench/gameplay.txt", "skill_formula_ns", "le", "3000"),
          ("bench/gameplay.txt", "script_total_cpu_percent", "le", "10")],
 artifacts=["config/gameplay/scripts.json", "scripting/gameplay/skill/fireball.lua"],
),
# ------------------------------------------------------------------ 034
dict(
 id="TASK-034", name="Client Core", phase="Phase 8 · 客户端",
 objective="实现客户端核心：GameLoop / Network / Input / Scene / Entity / Protocol / Config。**客户端网络协议直接使用 TASK-005 定义的协议，不另起一套。**",
 deps="TASK-005",
 module="client/core",
 notice=[
  '**⛔ 前置阻塞（2026-09-10）：本任务规格已过期，禁止按现规格开工。**',
  '',
  '**冲突**：本任务按「**自研 C++ 客户端**（自研主循环 / 窗口管理 / 系统层 / 帧调度）」编写，而客户端路线已于 **2026-08-29 批准为 Godot 4.7.1**（RFC `mmorpg_tasks/docs/rfc/client-engine-selection.md` §4.6 / §6.1），并于 **2026-09-10 追加形态约束：2D 优先，现阶段不做 3D 几何模型**（同 RFC **§9**）。',
  '',
  'Godot **自带**主循环、场景树、输入管理、窗口与生命周期管理 —— 本任务的交付物与之**全面重复**。另：本任务参考的「自研 D3D11 路线」已被否决（工作量最大，且 034~036 只给了 3 个任务的预算，属严重低估）。',
  '',
  '**处置**：① 本任务**不开工**，直到 RFC §9.8 的触发条件满足（先完成服务端与游戏核心 TASK-030~033 / 037 / 039 / 040 / 041）；② 届时按 RFC §6.1 + §9 用生成器**重写**本任务为 Godot 4.7.1 + 2D 规格后再执行；③ `client/` 目录当前为空（`find client -type f` 计数为 0），无既有实现需迁移，重写无沉没成本。',
  '',
  '**不变项**（重写时必须保留）：协议契约（TASK-005 的 FlatBuffers schema）、AOI Delta 与快照格式、通过 GDExtension(C++) 下沉协议与热路径的策略、`client/{runtime,network,gameplay,ui,extensions}` 目录分层与单向依赖约束、以及「**逻辑层不得解算表现**」这条纪律（RFC §9.6）。',
 ],
 owner=OWNER,
 inp="TASK-005 Protocol Schema（唯一契约）；注意：客户端**不依赖**任何服务端模块，只通过 Protocol 衔接",
 out="client/core 模块 + 连接网关跑通 + 客户端帧循环",
 iface="""```cpp
namespace mmo::client {
struct FrameContext { float dt_seconds; uint64_t frame_number; core::SteadyTime now;
                      core::Arena& frame_arena; };
class GameLoop { public:                     // 固定逻辑帧 + 可变渲染帧
  struct Config { uint32_t logic_hz{60}; uint32_t max_catchup{5}; bool vsync{true}; };
  core::Result<void> RegisterSystem(SystemPhase, std::function<void(const FrameContext&)>);
  core::Result<void> Run(); void Stop() noexcept;
  LoopStats Stats() const noexcept; };        // fps / logic_fps / frame_ms / longest_frame_ms
class NetClient { public:                    // 只实现客户端侧，不复用服务端传输模块
  core::Result<void> Connect(std::string_view addr, uint16_t port);
  core::Result<void> Send(uint32_t opcode, std::span<const uint8_t>);
  core::Result<void> Poll(std::vector<NetEvent>& out);   // 主线程驱动，禁止自带线程
  ConnectionState State() const noexcept; uint32_t RttMs() const noexcept; };
class ClientWorld { public:                  // 服务端状态的本地镜像 + 插值
  core::Result<void> ApplySnapshot(const protocol::SnapshotFrame&);  // FlatBuffers
  core::Result<void> Interpolate(const FrameContext&);               // 位置平滑
  entity::EntityId LocalPlayer() const noexcept; };
}
```""",
 data="""**客户端架构分层（与服务端严格解耦）**

```
Client:  Core → Network → World → Renderer → Resource
Server:  Core → Communication → Gateway → GameNode → Gameplay → Data
                    └──── 只通过 Protocol 契约衔接 ────┘
```

**帧模型**：逻辑帧固定 60Hz（可配），渲染帧跟随 vsync。网络快照到达后进入**插值缓冲**（默认 100ms 延迟），平滑播放。
**状态镜像**：客户端只持有服务端下发的镜像快照，**不做权威判定**（伤害、命中均由服务端裁定）。""",
 thread="主线程：GameLoop + 输入；网络线程：只做收发搬运（**不解析业务**）；渲染线程：TASK-035 引入。跨线程只传不可变快照，禁止共享可变 Entity。",
 hot="YES（帧循环与插值在客户端热路径）", io="YES（网络与配置）", rpc="NO（用自定义二进制协议，不是 gRPC）", persist="NO",
 state="本地镜像状态由 ClientWorld 拥有（主线程）；网络接收缓冲由网络线程拥有，Poll 时交接所有权。禁止客户端判定任何权威状态。",
 files=["client/core/include/mmo/client/", "client/core/src/", "client/core/tests/", "client/core/docs/", "config/client/"],
 steps=[
  "实现 game_loop.h/.cpp：固定逻辑步长 + CatchUp 限幅（max_catchup=5），帧耗时统计（最长帧、P95 帧）",
  "实现 net_client.h/.cpp：基于 TASK-005 协议的客户端收发，主线程 Poll（网络线程只搬运）",
  "实现连接管理：连接/重连/心跳/超时，状态机 Connecting→Connected→Reconnecting→Disconnected",
  "实现 protocol 复用：直接链接 protocol 模块的编解码库（Protobuf + FlatBuffers），**禁止重写一套**",
  "实现 input.h：输入采集与映射（键鼠/手柄抽象），输入只在逻辑帧开始采样一次",
  "实现 client_world.h/.cpp：快照应用 + 实体镜像 + 位置插值（100ms 缓冲，防抖动）",
  "实现插值与外推：位置用线性插值，短时丢包用外推（上限 200ms，超出则冻结）",
  "实现 config：客户端配置（config/client/*.json）—— 分辨率、帧率上限、网络参数、画质档位占位",
  "写测试：帧循环精度（60Hz 跑 10 秒误差 < 100ms）；插值正确性（两点之间位置线性）；断连重连；协议往返（与测试用服务端替身）",
  "写集成测试：起一个协议替身服务端，客户端连接 → 登录 → 收到快照 → 客户端渲染数据可用",
 ],
 unit="GameLoop 帧精度与 CatchUp 限幅；NetClient 状态机；协议编解码往返（复用 protocol）；插值/外推计算；输入采样；配置加载",
 integ="客户端连接测试替身服务端：完成连接 → 心跳 → 接收 100 帧快照 → 本地镜像实体数与服务端一致 → 插值后位置连续无跳变；中途断网 3 秒 → 自动重连 → 状态恢复",
 bench="bin/client_bench：`frame_ms_p95=` / `snapshot_apply_ns=` / `interpolate_ns_per_entity=` / `mem_bytes_client_base=` / `net_poll_ns=`",
 fail="服务端不可达：连接失败并返回明确错误，不崩溃、不无限阻塞；网络中断：转 Reconnecting 并按退避重连，本地画面冻结而非崩溃；收到损坏快照：校验失败丢弃该帧并使用上一帧（不崩溃）；帧耗时突增（如 GC/磁盘）：CatchUp 限幅，不产生死亡螺旋；收到未知 opcode：记录并跳过，不断开连接",
 accept=[
  "**客户端协议直接复用 TASK-005**（grep：client 目录无独立的消息头定义）",
  "**客户端不依赖任何服务端模块**（链接检查：client 目标不 link server 库）",
  "帧循环 60Hz 精度误差 < 100ms/10 秒，CatchUp 限幅生效",
  "连接 → 登录 → 收快照 → 插值全链路跑通（集成测试）",
  "断网重连可用，画面冻结而非崩溃",
  "损坏快照被丢弃且不崩溃",
  "Debug / Release 双构建通过，ctest -R Client 全绿",
 ],
 forbid=[
  "禁止客户端另起一套消息协议（必须复用 protocol 模块）",
  "禁止客户端依赖服务端模块代码（只通过 Protocol 契约）",
  "禁止客户端做权威判定（伤害/命中/掉落均由服务端裁定）",
  "禁止网络线程解析业务包（只搬运）",
  "禁止无 CatchUp 限幅的帧循环（死亡螺旋）",
  "禁止损坏数据导致客户端崩溃",
 ],
 perf="逻辑帧 60Hz 稳定（P95 帧 < 16.6ms）；快照应用 1000 实体 < 1ms；插值 < 50ns/实体；客户端基础内存 < 150MB；网络 Poll < 100us。",
 deliver=["client/core/include/mmo/client/game_loop.h", "client/core/include/mmo/client/net_client.h",
          "client/core/include/mmo/client/client_world.h", "client/core/src/*.cpp", "client/core/tests/*",
          "config/client/*.json", "client/core/docs/INTERFACE.md", "client/core/docs/README.md"],
 ctest="Client", both_build=True,
 bench_bins=[("bin/client_bench", "--frames 600")],
 metrics=[("bench/client.txt", "frame_ms_p95", "le", "16.6"), ("bench/client.txt", "mem_bytes_client_base", "le", "157286400")],
 artifacts=["client/core/include/mmo/client/net_client.h", "config/client/client.json"],
),
# ------------------------------------------------------------------ 035
dict(
 id="TASK-035", name="Renderer", phase="Phase 8 · 客户端",
 objective="实现低复杂度渲染器：Camera / Mesh / Material / Texture / Animation / UI。目标 Low Poly + Simple Lighting + Low Draw Call + Static Batching + LOD，**不要把引擎复杂度做得太高**。",
 deps="TASK-034",
 module="client/renderer",
 notice=[
  '**⛔ 前置阻塞（2026-09-10）：本任务规格已过期，禁止按现规格开工。**',
  '',
  '**冲突**：本任务明确「选定后端 **D3D11**，预留 Vulkan 抽象层但不实现」+「**禁止引入重型第三方引擎**」→ 即**自研轻量渲染器**；而客户端路线已于 **2026-08-29 批准为 Godot 4.7.1**，并于 **2026-09-10 追加形态约束：2D 优先，现阶段不做 3D 几何模型**（RFC **§9**）。',
  '',
  '§9 已把本任务的规格改写为：**2D 渲染管线**——`TileMapLayer` + `Sprite2D` / `AnimatedSprite2D` + Y-sort 遮挡 + 2D 粒子 + `CanvasLayer` UI；**仅启用 Compatibility 渲染器单档**（OpenGL 3.3 / DX11），Forward+ 高档推迟到引入 3D 表现时再启用；**性能预算改 2D 口径**（DrawCall 与图集切换次数、同屏精灵数、Canvas 重绘面积 / 每帧填充率、纹理显存），不再考核三角面数 / LOD 级数 / 烘焙光照。',
  '',
  '**处置**：① 本任务**不开工**，直到 RFC §9.8 的触发条件满足（先完成服务端与游戏核心 TASK-030~033 / 037 / 039 / 040 / 041）；② 届时按 RFC §6.1 + §9 用生成器**重写**本任务为 Godot 4.7.1 + 2D 规格后再执行；③ `client/` 目录当前为空（`find client -type f` 计数为 0），无既有实现需迁移，重写无沉没成本。',
  '',
  '**不变项**（重写时必须保留）：协议契约（TASK-005 的 FlatBuffers schema）、AOI Delta 与快照格式、通过 GDExtension(C++) 下沉协议与热路径的策略、`client/{runtime,network,gameplay,ui,extensions}` 目录分层与单向依赖约束、以及「**逻辑层不得解算表现**」这条纪律（RFC §9.6）。',
 ],
 owner=OWNER,
 inp="TASK-034 ClientWorld（渲染数据源）；低配红线：4核 CPU / 4GB RAM / 1GB VRAM（开发目标，最终以实测为准）",
 out="client/renderer 模块 + 渲染基准 + Draw Call / 显存报告",
 iface="""```cpp
namespace mmo::client::render {
struct RenderStats { uint32_t draw_calls; uint32_t triangles; uint32_t materials;
                     size_t texture_memory_bytes; size_t mesh_memory_bytes;
                     float cpu_ms; float gpu_ms; uint32_t shader_switches; };
class Renderer { public:
  struct Config { Backend backend{Backend::D3D11}; uint32_t target_fps{60};
                  bool vsync{true}; QualityLevel quality{QualityLevel::Low};
                  uint32_t max_draw_calls{500}; uint32_t shadow_quality{0}; };
  core::Result<void> Init(Config, void* native_window);
  core::Result<void> Resize(uint32_t w, uint32_t h);
  core::Result<RenderStats> RenderFrame(const client::ClientWorld&, const Camera&);
  core::Result<void> SetQuality(QualityLevel);      // Low/Medium/High 热切换
  RenderStats LastStats() const noexcept; };
class Camera { public: void SetPerspective(float fov, float aspect, float near_z, float far_z);
  void LookAt(const Vec3& eye, const Vec3& target); Mat4 ViewProj() const;
  Frustum GetFrustum() const; };                    // 视锥剔除必需
class MaterialSystem { public:
  core::Result<MaterialId> Create(const MaterialDesc&);   // 统一材质，减少切换
  core::Result<void> SetTexture(MaterialId, TextureSlot, TextureId); };
}
```""",
 data="""**渲染预算（Low 档，目标值，最终以实测为准）**

| 指标 | Low 档目标 |
|---|---|
| Draw Calls | < 300 |
| Triangles | < 300k |
| 纹理显存 | < 512MB |
| 网格显存 | < 256MB |
| Shader 切换 | < 50/帧 |
| 光照 | 1 方向光 + 顶点色烘焙，无实时阴影 |

**关键技术**：静态合批（静态物体按材质合批）、视锥剔除、LOD（3 级）、材质统一（减少切换）、实例化（同模型多实例）。
**UI**：独立 UI 层，用正交相机 + 图集合批，UI Draw Call < 20。""",
 thread="渲染线程独立于逻辑线程；逻辑线程产出不可变渲染快照，渲染线程消费（双缓冲）。禁止渲染线程访问 ClientWorld 可变状态。",
 hot="YES（每帧执行）", io="YES（加载纹理/网格，异步）", rpc="NO", persist="NO",
 state="渲染资源（纹理/网格/材质）由 ResourceManager（TASK-036）拥有并管理生命周期；渲染线程只持有句柄。",
 files=["client/renderer/include/mmo/client/render/", "client/renderer/src/", "client/renderer/tests/", "client/renderer/benchmark/", "config/client/render.json"],
 steps=[
  "选定后端：D3D11（低配目标 GPU 为传统 DX11），预留 Vulkan 抽象层接口但不实现",
  "实现 renderer.h/.cpp：初始化、Resize、RenderFrame、SetQuality",
  "实现 camera.h：透视/正交、视锥体（六个平面）与剔除测试",
  "实现 mesh / material / texture 三个资源句柄与加载（异步加载，主线程不阻塞）",
  "实现静态合批：静态物体按材质分组，合并顶点缓冲（构建期离线 + 运行期按区块）",
  "实现视锥剔除 + LOD 选择（3 级：近/中/远，按距离与屏占比）",
  "实现简单光照：1 个方向光 + 环境光 + 顶点色，**不做 PBR、不做实时阴影**",
  "实现实例化渲染：同模型多实体一次 Draw Call（用于怪物群、植被）",
  "实现 UI 层：正交相机 + 图集合批 + 文本渲染（位图字体，禁用复杂排版引擎）",
  "实现渲染统计：draw_calls / triangles / texture memory / shader switches / cpu_ms / gpu_ms",
  "写测试：视锥剔除正确性（已知位置集合的可见性断言）；LOD 选择；合批后 Draw Call 数下降；UI 批次数；画质热切换",
 ],
 unit="Camera 视锥构造与剔除；LOD 三级选择阈值；材质创建与纹理绑定；合批分组算法；UI 图集与批次；画质切换；统计字段准确",
 integ="渲染一个测试场景（1000 个静态物体 + 200 个动态实体 + 50 个 UI 元素）：Low 档下 Draw Call < 300、三角面 < 300k、纹理显存 < 512MB；画质从 Low 切到 High 不崩溃且显存变化可测；连续渲染 10 分钟无显存泄漏（显存占用曲线平稳）",
 bench="bin/render_bench：`draw_calls=` / `triangles=` / `texture_mem_mb=` / `mesh_mem_mb=` / `cpu_ms=` / `gpu_ms=` / `fps_p95=` / `shader_switches=`",
 fail="显存不足（注入 2GB 纹理）：按 LRU 降级/拒绝加载，返回明确错误而非崩溃；设备丢失（DXGI DEVICE_REMOVED）：捕获并尝试重建设备；窗口最小化/尺寸为 0：跳过渲染不崩溃；着色器编译失败：记录并使用默认材质，不黑屏崩溃；纹理加载失败：使用占位纹理（洋红）并告警",
 accept=[
  "Camera / Mesh / Material / Texture / Animation / UI 六项全部实现",
  "**Low 档 Draw Call < 300、三角面 < 300k、纹理显存 < 512MB**（benchmark 实测）",
  "静态合批与视锥剔除生效（有对照数据：开启前后 Draw Call 对比）",
  "LOD 三级生效",
  "画质 Low/Medium/High 可热切换",
  "连续渲染 10 分钟无显存泄漏",
  "设备丢失可恢复，不崩溃",
  "Debug / Release 双构建通过，ctest -R Renderer 全绿",
 ],
 forbid=[
  "禁止实现 PBR / 实时阴影 / 后处理管线（第一版不做）",
  "禁止无 Draw Call 预算地堆特效",
  "禁止渲染线程访问 ClientWorld 可变状态",
  "禁止主线程同步加载大纹理（必须异步）",
  "禁止在无显存回收策略下无限加载资源",
  "禁止引入重型第三方引擎（保持轻量）",
  "禁止在未实测前宣称支持某具体硬件",
 ],
 perf="Low 档：Draw Call < 300、三角面 < 300k、纹理显存 < 512MB、网格显存 < 256MB、Shader 切换 < 50/帧、CPU 渲染耗时 < 4ms、目标 60 FPS。**最终最低配置与实测 FPS 由 TASK-038 的客户端基准确定，本任务不得宣称兼容某硬件。**",
 deliver=["client/renderer/include/mmo/client/render/renderer.h", "client/renderer/include/mmo/client/render/camera.h",
          "client/renderer/include/mmo/client/render/material_system.h", "client/renderer/src/*.cpp",
          "client/renderer/tests/*", "client/renderer/benchmark/*", "config/client/render.json",
          "client/renderer/docs/INTERFACE.md", "client/renderer/docs/PERFORMANCE.md"],
 ctest="Renderer", both_build=True,
 bench_bins=[("bin/render_bench", "--scene test_scene --quality low --duration 600")],
 metrics=[("bench/render_low.txt", "draw_calls", "le", "300"),
          ("bench/render_low.txt", "texture_mem_mb", "le", "512"),
          ("bench/render_low.txt", "triangles", "le", "300000")],
 artifacts=["client/renderer/include/mmo/client/render/renderer.h", "client/renderer/docs/PERFORMANCE.md"],
),
# ------------------------------------------------------------------ 036
dict(
 id="TASK-036", name="Resource / Low Spec System", phase="Phase 8 · 客户端",
 objective="这是「老电脑能跑」的核心任务：ResourceManager / TextureCache / MeshCache / AudioCache / SceneStreaming / ChunkLoading / LOD，支持 Low / Medium / High 三档。**地图不能全部常驻内存：只保留 Current Chunk + Nearby Chunk，远处自动释放。**",
 deps="TASK-034, TASK-035",
 module="client/resource",
 notice=[
  '**⛔ 前置阻塞（2026-09-10）：本任务规格已过期，禁止按现规格开工。**',
  '',
  '**冲突**：本任务按「**自研资源系统**（自研 Asset 加载器 / 自研纹理与网格缓存 / 自研 Chunk Streaming）」编写，且 §1 的目标是「Low Poly + 三档画质 + 3D 地图分块常驻」；而客户端路线已于 **2026-08-29 批准为 Godot 4.7.1**，并于 **2026-09-10 追加 2D 约束**（RFC **§9**）。Godot 自带 `ResourceLoader` / `.import` 管线 / 引用计数资源缓存，本任务交付物与之全面重复；3D 网格缓存与 Chunk Streaming 在 2D 阶段**不适用**。',
  '',
  '§9 已把本任务的规格改写为：**2D 资源管线**——图集（atlas）+ `.import` 配置 + 音频常驻/流式划分；**地图分块加载改为 `TileMap` 区块**（替代 3D Chunk Streaming）；三档画质按 2D 口径重定义；Low 档（4 核 / 4GB RAM / 1GB VRAM）目标从「激进值」变为**宽松值**，但**仍需实测容量报告**。',
  '',
  '**处置**：① 本任务**不开工**，直到 RFC §9.8 的触发条件满足（先完成服务端与游戏核心 TASK-030~033 / 037 / 039 / 040 / 041）；② 届时按 RFC §6.1 + §9 用生成器**重写**本任务为 Godot 4.7.1 + 2D 规格后再执行；③ `client/` 目录当前为空（`find client -type f` 计数为 0），无既有实现需迁移，重写无沉没成本。',
  '',
  '**不变项**（重写时必须保留）：协议契约（TASK-005 的 FlatBuffers schema）、AOI Delta 与快照格式、通过 GDExtension(C++) 下沉协议与热路径的策略、`client/{runtime,network,gameplay,ui,extensions}` 目录分层与单向依赖约束、以及「**逻辑层不得解算表现**」这条纪律（RFC §9.6）。',
 ],
 owner=OWNER,
 inp="TASK-034 ClientWorld；TASK-035 Renderer；低配红线（4核/4GB RAM/1GB VRAM，开发目标非承诺）",
 out="client/resource 模块 + 流式加载 + 三档配置 + 资源占用报告",
 iface="""```cpp
namespace mmo::client::resource {
enum class QualityLevel : uint8_t { Low=0, Medium=1, High=2 };
struct QualityPreset {                        // 三档全部配置化，禁止硬编码
  QualityLevel level; uint32_t texture_max_size; uint32_t anisotropic;
  float lod_bias; uint32_t shadow_quality; uint32_t max_visible_entities;
  uint32_t max_particles; uint32_t net_update_hz; float view_distance;
  uint32_t chunk_radius;                      // 保留几个邻近区块
  size_t texture_budget_bytes; size_t mesh_budget_bytes; };
class ResourceManager { public:
  core::Result<void> Init(const QualityPreset&);
  core::Result<ResourceHandle> LoadAsync(std::string_view uri, ResourceType);
  core::Result<void> Unload(ResourceHandle);
  core::Result<void> SetQuality(QualityLevel);      // 热切换 + 按新预算回收
  ResourceStats Stats() const noexcept; };
class SceneStreamer { public:                 // 分块流式加载
  struct Config { float chunk_size{128.0f}; uint32_t load_radius{2};   // 当前 + 周围 2 圈
                  uint32_t unload_delay_seconds{5}; uint32_t max_pending_loads{4}; };
  core::Result<void> Update(const Vec3& player_pos, const FrameContext&);
  core::Result<void> ForceUnloadAll();
  StreamingStats Stats() const noexcept; };   // loaded_chunks / pending / last_load_ms
}
```""",
 data="""**三档预设（配置化，config/client/quality.json）**

| 项 | Low | Medium | High |
|---|---|---|---|
| 纹理上限 | 512 | 1024 | 2048 |
| LOD Bias | +1.0 | 0 | 0 |
| 阴影 | 关 | 低 | 中 |
| 同屏实体 | 50 | 150 | 300 |
| 粒子数 | 200 | 1000 | 3000 |
| 网络更新率 | 10Hz | 20Hz | 30Hz |
| 视距 | 80m | 150m | 250m |
| 区块半径 | 1 | 2 | 3 |
| 纹理预算 | 256MB | 512MB | 1GB |
| 网格预算 | 128MB | 256MB | 512MB |

**内存硬约束（Low 档目标）**：总 RAM < 1.5GB、VRAM < 1GB、Draw Call < 300、加载时间 < 15s。
**Chunk 策略**：`Current Chunk + Nearby Chunk`，进入加载半径异步加载，超出卸载半径延迟 5 秒释放（防来回抖动）。""",
 thread="资源加载在 Worker 线程池（2~4 线程，按 CPU 核数）；卸载在主线程安全点执行（无渲染引用时）；Streamer 在主线程 Update，按帧预算限流（每帧最多 1 个 chunk 加载完成回调）。",
 hot="YES（每帧 Streaming 检查）", io="YES（磁盘/包体读取）", rpc="NO", persist="NO",
 state="资源句柄与缓存表由 ResourceManager 拥有（主线程 + 内部锁保护引用计数）；已加载 Chunk 集合由 SceneStreamer 拥有。释放必须等渲染线程无引用。",
 files=["client/resource/include/mmo/client/resource/", "client/resource/src/", "client/resource/tests/",
        "client/resource/benchmark/", "config/client/quality.json"],
 steps=[
  "定义 quality_preset.h：三档预设结构与配置加载（config/client/quality.json）",
  "实现 resource_manager.h/.cpp：异步加载、引用计数、LRU 回收、预算超限强制回收",
  "实现三个缓存：TextureCache / MeshCache / AudioCache，各自独立预算（防止一类资源吃满）",
  "实现 scene_streamer.h/.cpp：按玩家位置的九宫格/半径 Chunk 加载与延迟卸载",
  "实现加载限流：每帧最多处理 N 个加载完成回调（防卡顿尖峰），pending 队列有上限",
  "实现防抖动：Chunk 进入/离开边界加迟滞（hysteresis），快速来回移动不触发反复加载",
  "实现画质热切换：切档后按新预算回收（先回收纹理，再回收网格），并重建 LOD 距离",
  "实现资源统计面板数据：RAM / VRAM / Draw Call / 实体数 / 粒子数 / 加载队列长度 / 最近加载耗时",
  "实现 LOD 联动：Streamer 与渲染 LOD 共享 QualityPreset，距离阈值统一来源",
  "写测试：LRU 回收；预算超限强制回收；Chunk 加载/卸载边界；迟滞防抖；画质切换后资源占用变化；异步加载不卡主线程",
  "写**低配实测脚本 tools/lowspec/profile.py**：采集 RAM/VRAM/FPS/DrawCall 曲线并出报告",
 ],
 unit="QualityPreset 加载与校验；ResourceManager 引用计数与 LRU；三类缓存独立预算；Chunk 加载/卸载判定（含迟滞）；加载限流；画质切换后回收；统计字段",
 integ="**长距离移动测试**：玩家以最高速度在地图上跑 5 分钟，验证：常驻 Chunk 数稳定在配置值（不无限增长）、RAM 曲线平稳（不单调上升）、无卡顿尖峰（最长帧 < 100ms）、远处 Chunk 被正确释放；来回穿越 Chunk 边界 50 次，加载次数不爆炸（迟滞生效）",
 bench="bin/resource_bench + tools/lowspec/profile.py：`ram_mb=` / `vram_mb=` / `draw_calls=` / `load_ms_p95=` / `chunks_loaded=` / `cache_hit_rate=` / `fps_p95=`",
 fail="磁盘资源缺失：使用占位资源 + 告警，不崩溃；加载队列打满：丢弃最远 Chunk 请求并计数；显存不足：按 LRU 强制回收（先卸载远距离纹理），若仍不足则拒绝加载并降级画质；玩家瞬移（跨 50 个 Chunk）：分批加载（限流），不产生单帧 10 秒卡顿；画质切到 Low 时显存未及时回落：强制回收并在 2 秒内达标",
 accept=[
  "**Current Chunk + Nearby Chunk 策略生效**，远处自动释放（集成测试：5 分钟跑图 RAM 平稳）",
  "Low / Medium / High 三档全部可用且可热切换",
  "**Low 档 RAM < 1.5GB、VRAM < 1GB、Draw Call < 300**（实测，写入 PERFORMANCE.md）",
  "三类缓存各自独立预算，互不挤占（单测）",
  "来回穿越 Chunk 边界不触发加载风暴（迟滞生效）",
  "低配实测脚本可输出完整资源曲线报告",
  "Debug / Release 双构建通过，ctest -R Resource 全绿",
 ],
 forbid=[
  "禁止地图全部常驻内存（必须分块流式）",
  "禁止无预算上限的资源缓存",
  "禁止主线程同步加载资源",
  "禁止画质切到 Low 后显存不回落",
  "禁止硬编码画质参数（必须配置化）",
  "禁止在未实测前宣称支持 4核/4GB/1GB VRAM 具体硬件",
  "禁止通过提高硬件要求来回避性能问题",
 ],
 perf="Low 档：RAM < 1.5GB、VRAM < 1GB、Draw Call < 300、同屏实体 ≤ 50、粒子 ≤ 200、场景加载 < 15s、FPS P95 ≥ 30（**最终数字以 TASK-038 实测为准**）。",
 deliver=["client/resource/include/mmo/client/resource/resource_manager.h",
          "client/resource/include/mmo/client/resource/scene_streamer.h",
          "client/resource/include/mmo/client/resource/quality_preset.h",
          "client/resource/src/*.cpp", "client/resource/tests/*", "client/resource/benchmark/*",
          "config/client/quality.json", "tools/lowspec/profile.py",
          "client/resource/docs/INTERFACE.md", "client/resource/docs/PERFORMANCE.md"],
 ctest="Resource", both_build=True,
 bench_bins=[("bin/resource_bench", "--quality low --duration 300")],
 metrics=[("bench/resource_low.txt", "ram_mb", "le", "1536"),
          ("bench/resource_low.txt", "vram_mb", "le", "1024"),
          ("bench/resource_low.txt", "draw_calls", "le", "300")],
 artifacts=["config/client/quality.json", "client/resource/include/mmo/client/resource/scene_streamer.h"],
),
# ------------------------------------------------------------------ 037
dict(
 id="TASK-037", name="Reconnect / Failover / Scene Recovery", phase="Phase 9 · 容灾",
 objective="实现节点注册与健康、玩家重连、GameNode 故障接管、第一版 Scene 恢复。**第一版只要求恢复最近一次可靠持久化状态，Live Scene Migration 留到第二阶段（已登记 RFC：docs/rfc/scene-live-migration.md）。** 注意：根规范 §1 列「Scene 可以独立迁移」为长期目标，与 §34「第一版只要求重连+恢复」存在张力，本任务按 §34 执行，目标 §1 的措辞待规范层澄清。",
 deps="TASK-009, TASK-010, TASK-012, TASK-026, TASK-027",
 module="server/gateway + server/gamenode",
 owner=OWNER,
 inp="TASK-009 Session（含 version 防回放）；TASK-010 NodeRegistry；TASK-012 Scene；TASK-026/027 数据服务",
 out="容灾模块 + 四个子项全部实现 + 故障演练报告",
 iface="""```cpp
namespace mmo::resilience {
// 37.1 Node Registry
struct NodeHealth { NodeId id; core::SteadyTime last_heartbeat; uint32_t missed;
                    uint32_t load; NodeStatus status; };   // Healthy/Suspect/Dead
class HealthMonitor { public:
  core::Result<void> Tick(core::SteadyTime now);           // 心跳扫描
  core::Result<std::vector<NodeId>> FindReplacement(NodeRole, std::string_view affinity);
  NodeStatus StatusOf(NodeId) const noexcept; };
// 37.2 Player Reconnect
enum class ReconnectStep : uint8_t { Disconnected, Reconnecting, Authenticating,
                                     LoadingPlayer, AttachingScene, Resumed, Failed };
class ReconnectService { public:
  core::Result<ReconnectStep> Begin(SessionId, net::ConnectionId, core::TraceID);
  core::Result<ReconnectStep> Advance(SessionId, core::TraceID);   // 状态机推进
  core::Result<void> OnComplete(SessionId); };
// 37.3 GameNode Failure
class FailoverCoordinator { public:
  core::Result<void> OnNodeDead(NodeId, core::TraceID);
  core::Result<void> ReattachPlayers(NodeId from, NodeId to, core::TraceID);
  FailoverStats Stats() const noexcept; };    // detected / reattached / failed / duration_ms
// 37.4 Scene Recovery
class SceneRecovery { public:
  core::Result<scene::Scene*> Restore(SceneId, NodeId new_owner, core::TraceID);
  core::Result<void> Checkpoint(const scene::Scene&);      // 定期可靠快照
  uint32_t LastCheckpointVersion(SceneId) const noexcept; };
}
```""",
 data="""**37.2 重连流程（六步，顺序固定）**

```
Disconnect → Reconnect → Authenticate → Load Player → Attach Scene → Resume
   │              │            │              │              │           │
   └─ 保留 Session（grace 期）  └─ 校验 version    └─ DataService   └─ 绑定新 GameNode
```

**37.3 故障接管**：Gateway Detect（3 次心跳丢失判 Dead）→ Find Replacement（低负载同角色节点）→ Player Reattach（按 Session 重建映射）→ 恢复最近一次可靠状态。

**37.4 Scene 恢复第一版**：从最近一次 Checkpoint（默认 30 秒一次）或玩家持久化数据恢复。**明确不做** Live Scene Migration（保留实时状态迁移）—— 那是第二阶段。
**关键约束**：Redis 不作为实时状态的权威 Owner；恢复后玩家可能回到 30 秒前的位置，这是第一版可接受的设计。""",
 thread="HealthMonitor 由 Gateway 主线程 Tick 驱动；Reconnect/Failover 状态机在 Gateway 主线程；Scene 重建在目标 GameNode 的 SimulationThread。禁止跨线程共享会话状态。",
 hot="NO（故障路径，非每 Tick）", io="YES（读持久化状态）", rpc="YES（跨节点协调）", persist="YES",
 state="会话状态 Owner 是 Gateway（SessionManager）；节点健康 Owner 是 HealthMonitor；Scene 状态 Owner 在恢复后转移到新 GameNode（**唯一 Owner 转移，不允许双写**）。",
 files=["server/gateway/src/resilience/", "server/gateway/include/mmo/gateway/resilience/",
        "server/gamenode/scene/src/recovery/", "server/gamenode/scene/include/mmo/game/scene/recovery/",
        "server/gateway/tests/", "docs/"],
 steps=[
  "**37.1** 实现 health_monitor.h/.cpp：心跳扫描、Suspect/Dead 判定（3 次丢失）、替换节点选择（一致性哈希 + 负载）",
  "**37.1** 实现节点健康指标：missed 心跳数、判定耗时、Dead 节点计数",
  "**37.2** 实现 reconnect_service.h/.cpp：六步状态机，每步可单独失败与重试",
  "**37.2** 实现 version 校验：expected_version 不匹配则拒绝（防旧连接回放，复用 TASK-009）",
  "**37.2** 实现 grace 期内重连：Session 保留，玩家数据从缓存/DataService 载入",
  "**37.3** 实现 failover_coordinator.h/.cpp：OnNodeDead → 批量失效路由 → 选新节点 → 批量 Reattach",
  "**37.3** 实现批量限速：单次故障最多并发迁移 N 个玩家（防雪崩），其余排队",
  "**37.4** 实现 Checkpoint：Scene 每 30 秒（可配）产出可靠快照（只存玩家与关键 NPC 状态，不存全部实体）",
  "**37.4** 实现 Restore：从 Checkpoint 或玩家持久化数据重建 Scene，恢复后广播位置纠正",
  "实现故障演练脚本 tools/chaos/kill_gamenode.sh（本任务先做 GameNode 单项）",
  "写四个子项各自的测试 + 端到端故障演练",
 ],
 unit="HealthMonitor 心跳与状态判定；替换节点选择；重连六步状态机各步成功/失败；version 校验；Checkpoint 产出与解析；Restore 后字段正确；批量限速",
 integ="""**端到端故障演练（必须真跑）**

| 演练 | 操作 | 断言 |
|---|---|---|
| 玩家重连 | kill 客户端连接，5 秒内重连 | 六步走完，玩家回到原 Scene，背包/属性一致 |
| GameNode 崩溃 | kill -9 一个 GameNode 进程 | Gateway 在 15 秒内检测到，玩家被分配到新节点，全部可继续游戏 |
| 会话版本攻击 | 用旧 version 请求 Reattach | 被拒绝，不影响新连接 |
| Scene 恢复 | 崩溃后恢复 Scene | 玩家状态 = 最近一次 Checkpoint，无数据损坏（无重复物品/无负余额）""",
 bench="bin/resilience_bench：`detect_ms=`（Dead 判定耗时）/ `reattach_ms_per_player=` / `scene_restore_ms=` / `checkpoint_ms=` / `checkpoint_bytes=`",
 fail="Gateway 自身崩溃：玩家无法重连（可接受，但必须有明确日志与告警，且 Gateway 需多实例）；目标 GameNode 也不可用：重连进入 Failed 并给客户端明确提示，禁止无限重试；Checkpoint 写入失败：保留上一个 Checkpoint 并告警；玩家数据在恢复时被并发修改：版本校验拦截并重试；批量故障（1000 玩家同时重连）：限速队列生效，不雪崩",
 accept=[
  "**37.1 / 37.2 / 37.3 / 37.4 四个子项全部实现**，各有独立测试",
  "端到端演练四项全部跑通（见上表，需真实 kill 进程）",
  "GameNode 崩溃后 15 秒内检测 + 玩家可继续游戏",
  "version 校验拒绝旧连接回放（单测）",
  "恢复后无数据损坏：无重复物品、无负余额、无属性错乱（对账校验）",
  "批量重连限速生效，不雪崩",
  "**明确记录**：第一版不做 Live Scene Migration，恢复可能回退到最近 Checkpoint（写入文档）",
  "Debug / Release 双构建通过，ctest -R Resilience 全绿",
 ],
 forbid=[
  "禁止把 Redis 当作实时状态的权威 Owner",
  "禁止第一版实现 Live Scene Migration（第二阶段再做）",
  "禁止重连时不校验 version（会回放旧连接）",
  "禁止故障转移时出现双 Owner 同时写同一 Scene",
  "禁止无限重试导致雪崩（必须限速 + 退避）",
  "禁止用 kill 之外的“模拟“代替真实故障演练",
  "禁止把 Checkpoint 失败静默吞掉",
 ],
 perf="Dead 判定 < 15 秒（3 × 5s 心跳）；单玩家重连 < 500ms；1000 玩家批量接管 < 60 秒；Scene 恢复 < 3 秒；Checkpoint 产出 < 100ms、大小 < 1MB/Scene。",
 deliver=["server/gateway/include/mmo/gateway/resilience/health_monitor.h",
          "server/gateway/include/mmo/gateway/resilience/reconnect_service.h",
          "server/gateway/include/mmo/gateway/resilience/failover_coordinator.h",
          "server/gamenode/scene/include/mmo/game/scene/recovery/scene_recovery.h",
          "server/gateway/src/resilience/*.cpp", "server/gamenode/scene/src/recovery/*.cpp",
          "server/gateway/tests/*", "tools/chaos/kill_gamenode.sh", "docs/failover-drill-report.md"],
 ctest="Resilience", both_build=True,
 bench_bins=[("bin/resilience_bench", "--players 1000")],
 metrics=[("bench/resilience.txt", "detect_ms", "le", "15000"),
          ("bench/resilience.txt", "reattach_ms_per_player", "le", "500")],
 artifacts=["server/gateway/include/mmo/gateway/resilience/failover_coordinator.h", "docs/failover-drill-report.md"],
),
# ------------------------------------------------------------------ 038
dict(
 id="TASK-038", name="Bot + Load + Chaos + Delivery", phase="Phase 10 · 最终工程验收",
 objective="最终工程验证平台：Bot 框架、逐级 CCU 压测、网络模拟、Chaos 测试、最终交付包。**这是整个项目能否交付的判定点。**",
 deps="TASK-025, TASK-027, TASK-028, TASK-034, TASK-036, TASK-037, TASK-039, TASK-040, TASK-041",
 module="tools/qa + docs/architecture",
 owner=OWNER,
 inp="TASK-025 战斗基准；TASK-034/036 客户端；TASK-037 容灾；TASK-027/028 数据服务；TASK-039 Social 运行时；TASK-040 ControlService 控制面；TASK-041 跨进程集成与战斗回归",
 out="Bot 框架 + 压测套件 + Chaos 套件 + 最终交付包（架构文档/源码/docker/k8s/schema/proto/lua/tests/benchmarks/tools/docs）",
 iface="""```cpp
// 38.1 Bot Framework（无渲染，纯协议层）
namespace mmo::bot {
enum class BotAction : uint8_t { Login, Move, Attack, Quest, Trade, Chat, Logout, Reconnect };
struct BotScript { std::vector<BotAction> actions; std::vector<DurationMs> delays; uint32_t loop{1}; };
class Bot { public:
  core::Result<void> Run(const BotScript&, std::string_view gateway_addr);
  BotStats Stats() const noexcept; };        // actions_done / errors / rtt_ms_p95 / received_pps
class BotFarm { public:                      // 单机启动 N 个 Bot
  core::Result<void> Spawn(uint32_t count, const BotConfig&);
  core::Result<AggregateStats> RunUntil(DurationMs);
  core::Result<void> StopAll(DurationMs grace); };
}
// 38.2~38.4 由 tools/ 下的脚本编排，非 C++ 接口
```""",
 data="""**38.2 CCU 阶梯（必须逐级跑完，禁止跳级）**

| 级别 | 通过条件 |
|---|---|
| 100 | Tick P99 ≤ 8ms、错误率 < 0.1% |
| 500 | 同上 |
| 1000 | 同上 |
| 5000 | 同上 + 单 GameNode 承受 |
| 10000 | 多 GameNode 水平扩容 |
| 20000 | 水平扩容 + 数据层无瓶颈 |
| 50000 | 最终目标；**不达标则记录容量上限，不宣称达成** |

**38.3 网络模拟**：Latency(50/100/200ms) / Jitter(±20ms) / Packet Loss(1%/5%) / Bandwidth Limit / Disconnect / Reconnect。
**38.4 Chaos**：GameNode Crash / Gateway Crash / Redis Failure / MySQL Failure / Network Partition / CPU Saturation / Memory Pressure —— **七项全部真跑**。
**38.5 交付清单**：architecture/{architecture.md, state-ownership.md, dependency.md, sequence/, deployment/, capacity-report.md} + source / docker / k8s / schema / proto / flatbuffers / lua / tests / benchmarks / tools / docs。""",
 thread="Bot 进程独立于服务进程，可多机部署；每个 Bot 单线程事件驱动（支持单机数千连接）。压测期间禁止在被测机器上跑 Bot（会污染数据）。",
 hot="NO（测试工具）", io="YES（写报告）", rpc="YES（压测流量）", persist="NO",
 state="Bot 自身状态由各 Bot 实例拥有（无共享）；压测数据汇总到独立的采集器进程。",
 files=["tools/bot/", "tools/load/", "tools/chaos/", "tools/netsim/", "tools/report/",
        "docs/architecture/", "deploy/docker/", "deploy/k8s/"],
 steps=[
  "**38.1** 实现 Bot：协议层客户端（复用 TASK-005），八种行为（Login/Move/Attack/Quest/Trade/Chat/Logout/Reconnect）",
  "**38.1** 实现 BotFarm：单机启动 N 个 Bot，资源占用可控（目标单机 5000 Bot）",
  "**38.2** 实现压测编排 tools/load/run_ladder.sh：按 100/500/1000/5000/10000/20000/50000 逐级，采集 CPU/RAM/Network/Tick/P95/P99/Redis OPS/MySQL QPS/Error Rate",
  "**38.2** 实现指标采集器：从各进程拉取指标，统一写入时序文件（CSV/JSON）",
  "**38.3** 实现网络模拟：接入 tc/netsh 或代理层，支持延迟/抖动/丢包/带宽/断连/重连六种",
  "**38.4** 实现 Chaos 套件 tools/chaos/：七种故障注入脚本（kill 进程 / 停容器 / 网络分区 / CPU 打满 / 内存压力）",
  "**38.4** 每个 Chaos 用例必须有**明确的预期行为与恢复断言**（不是“看看会不会崩“）",
  "**38.5** 编写 docs/architecture/ 六份文档：architecture.md / state-ownership.md / dependency.md / sequence/ / deployment/ / capacity-report.md",
  "**38.5** 整理交付包：docker-compose、k8s manifests、schema（SQL 迁移）、proto、flatbuffers、lua 脚本、tests、benchmarks、tools、docs",
  "**38.5** 编写 README / DEPLOYMENT / OPERATIONS 三份运维文档",
  "跑完整阶梯压测与七项 Chaos，生成最终报告",
 ],
 unit="Bot 八种行为各自可用；BotScript 解析与循环；BotFarm 批量启停；指标采集字段完整；报告生成格式正确",
 integ="""**最终验收（全部真跑，禁止模拟代替）**

1. CCU 阶梯 7 级全部跑完，每级数据完整（未达标要记录实际容量上限）
2. 七项 Chaos 全部执行，每项有恢复断言与耗时
3. 网络模拟六种场景，客户端表现符合预期（不外挂、不崩溃、可重连）
4. 交付包可在一台新机器上 `docker compose up` 后跑通 100 CCU 冒烟""",
 bench="bin/bot_bench + tools/load/：`ccu_level=` / `tick_p95_ms=` / `tick_p99_ms=` / `cpu_percent=` / `rss_mb=` / `redis_ops=` / `mysql_qps=` / `error_rate=` / `net_mbps=`",
 fail="压测中服务崩溃：记录崩溃点 CCU 与原因，该级判定失败（不得美化）；Chaos 后无法恢复：判定该用例失败并记录 RTO；网络极端丢包 5%：客户端应可玩（ degraded 但不断连），若断连则记录；容量不达 50000：如实记录实际容量上限与瓶颈（写入 capacity-report.md），**禁止宣称达成**",
 accept=[
  "**CCU 阶梯 7 级全部跑完**（100/500/1000/5000/10000/20000/50000），数据完整",
  "**七项 Chaos 全部执行**（GameNode Crash / Gateway Crash / Redis Failure / MySQL Failure / Network Partition / CPU Saturation / Memory Pressure），每项有恢复断言",
  "网络模拟六种场景全部覆盖",
  "docs/architecture/ 六份文档齐全",
  "交付包在新机器上可复现 100 CCU 冒烟",
  "capacity-report.md 如实记录容量上限（不达标不得宣称）",
  "全部历史 benchmark 数据汇总归档",
  "Debug / Release 双构建通过，全量 ctest 全绿",
 ],
 forbid=[
  "禁止跳级压测（必须逐级）",
  "禁止用模拟代替真实故障演练",
  "禁止在未达标时宣称支持 50000 CCU",
  "禁止 Chaos 用例没有恢复断言",
  "禁止在被测机器上跑 Bot（数据污染）",
  "禁止交付包缺少架构六文档中的任何一份",
  "禁止遗漏任何一项真实故障演练",
 ],
 perf="逐级 CCU 全部记录；目标 50000 CCU 下 Tick P99 ≤ 8ms、错误率 < 0.1%。**若未达标，capacity-report.md 必须写明实际容量上限、瓶颈组件与扩容建议，禁止宣称达成。**",
 deliver=["tools/bot/*", "tools/load/*", "tools/chaos/*", "tools/netsim/*", "tools/report/*",
          "docs/architecture/architecture.md", "docs/architecture/state-ownership.md",
          "docs/architecture/dependency.md", "docs/architecture/sequence/*",
          "docs/architecture/deployment/*", "docs/architecture/capacity-report.md",
          "deploy/docker/*", "deploy/k8s/*", "README.md", "DEPLOYMENT.md", "OPERATIONS.md"],
 ctest="Bot", both_build=True,
 bench_bins=[("bin/bot_bench", "--bots 1000 --duration 300")],
 metrics=[("bench/load_1000.txt", "tick_p99_ms", "le", "8"),
          ("bench/load_1000.txt", "error_rate", "le", "0.001")],
 artifacts=["docs/architecture/architecture.md", "docs/architecture/capacity-report.md", "tools/bot/bot.h"],
),
# ------------------------------------------------------------------ 039
dict(
 id="TASK-039", name="Social System（组队/好友/公会/聊天/邮件）", phase="Phase 7 · 社交系统",
 objective="实现社交运行时：Party（组队）/ Friend（好友）/ Guild（公会）/ Chat（聊天）/ Mail（邮件）。"
            "第一版**单 GameNode 内全量内存态**，跨节点社交由 Gateway 路由 + DataService 持久化副本兜底。"
            "所有社交动作走 Command / Event，禁止直接改 Role 或 Scene 数据。**这是对「类大型 MMORPG」功能完整度的补齐。**",
 deps="TASK-007, TASK-011, TASK-016, TASK-028",
 module="server/gamenode/social",
 owner=OWNER,
 inp="TASK-007 Command/Event Bus；TASK-011 Entity（玩家实体引用）；TASK-016 Role（玩家档案）；TASK-028 MySQL（Guild/Mail 表已存在）",
 out="Social 模块（Party/Friend/Guild/Chat/Mail）+ 社交事件定义 + 单 GameNode 集成测试 + 接口文档",
 iface="""```cpp
namespace mmo::game::social {
enum class SocialOp : uint8_t { Invite, Accept, Decline, Leave, Kick, Promote,
                                AddFriend, RemoveFriend, SendMail, ClaimMail, GuildCreate, GuildJoin };
struct Party { party_id; std::vector<player_id> members; player_id leader; scene_id home; };
struct Guild { guild_id; std::string name; player_id leader; uint32_t member_count; };
class SocialSystem { public:
  // 全部经 Command 进入，返回 Result；禁止内部直改 Role
  core::Result<party_id> CreateParty(player_id leader);
  core::Result<void>   Invite(player_id from, player_id to);
  core::Result<void>   AcceptInvite(player_id who, party_id pid);
  core::Result<void>   Leave(player_id who);
  core::Result<guild_id> CreateGuild(player_id leader, std::string_view name);
  core::Result<void>   JoinGuild(player_id who, guild_id gid);
  core::Result<void>   SendMail(player_id from, player_id to, std::string_view payload);
  // 事件统一发出：PartyEvent / GuildEvent / ChatEvent / MailEvent
};
}
```""",
 data="**内存模型（第一版，单 GameNode 内全量）**\n\n"
      "- Party：GameNode 内存 `unordered_map<party_id, Party>`，成员上限可配（默认 5）。\n"
      "- Friend：每玩家 `set<player_id>`，挂 Role 之下，持久化经 DataService。\n"
      "- Guild：内存 `unordered_map<guild_id, Guild>` + 成员表；权威持久化在 MySQL（TASK-028 表已建）。\n"
      "- Chat：频道（私聊/队伍/公会/世界）由 ChatService 在 GameNode 内路由；**世界频道走 Gateway 广播而非全服 O(N) 扫描**（复用 AOI 邻域或订阅表）。\n"
      "- Mail：写入经 Economy/Ledger 同款幂等通道，读取走 Query。\n\n"
      "**扩展点**：新增社交关系类型必须走 `SocialOp` 枚举 + 注册表，禁止在 handler 里硬编码穷举。",
 thread="SocialSystem 运行在 Scene/GameNode 主 Simulation 线程（同其它 Gameplay 模块），不新建线程；"
        "Guild 持久化写走 DataService 异步，热路径不触 MySQL。",
 hot="NO（社交事件低频）", io="YES（Guild/Mail 持久化经 DataService）", rpc="NO（第一版单 GameNode 内）", persist="YES（Guild/Mail 落 MySQL）",
 files=["server/gamenode/social/include/mmo/game/social/", "server/gamenode/social/src/",
        "server/gamenode/social/tests/", "server/gamenode/social/docs/INTERFACE.md",
        "config/gameplay/social.json"],
 steps=[
  "实现 SocialSystem 骨架与 `SocialOp` 枚举 + 注册表（禁止 switch 硬编码穷举）",
  "实现 Party：创建/邀请/接受/离开/踢人/队长转移；成员上限可配；状态写回 Role 经 Command",
  "实现 Friend：加/删好友，双向一致，持久化经 DataService",
  "实现 Guild：创建/加入/退出/成员管理/权限（leader/member）；内存态 + MySQL 持久化双写（异步）",
  "实现 Chat：私聊/队伍/公会/世界四频道；世界频道经 Gateway 订阅表而非全服扫描",
  "实现 Mail：发信/收信/领取附件（附件走 Economy 幂等通道，复用 TASK-030）；过期清理",
  "定义社交事件（PartyEvent/GuildEvent/ChatEvent/MailEvent）并入 EventBus（TASK-007）",
  "写 INTERFACE.md：导出头清单 + 消费的上游接口（TASK-007/011/016/028）",
  "集成测试：组队进本（跨 Scene 走 Command，复用 TASK-020）、公会创建+持久化往返、私聊端到端",
 ],
 unit="各社交操作的 Result 正确（重复邀请/已存在/权限不足等错误码）；Guild 双写一致性（内存==MySQL 快照）；事件触发计数正确",
 integ="单 GameNode 内：A 邀 B 组队→B 接受→两人同 Party；A 建公会→B 加入→MySQL 可见；A 私聊 B→B 收到 ChatEvent；邮件领取幂等（重复领取只发一次附件）",
 bench="无独立 benchmark（社交事件低频）；可统计单 GameNode 社交操作吞吐（> 1k ops/s）作为容量基线",
 fail="Guild 持久化失败：内存态保留、标记待重试、不丢数据；世界频道在高在线下不退化成全服 O(N) 广播；邮件重复领取：第二次返回 AlreadyClaimed，不重复发附件",
 accept=[
  "Party/Friend/Guild/Chat/Mail 五项全部可运行并集成测试通过",
  "所有社交写操作经 Command/Event，grep 确认无直接改 Role/Scene 私有成员",
  "Guild 内存态与 MySQL 持久化一致（集成测试断言往返）",
  "世界频道走 Gateway 订阅表，禁止出现全服玩家遍历广播（代码评审 + 静态审查）",
  "邮件领取幂等：重复领取只发一次附件（复用 TASK-030 幂等键）",
  "INTERFACE.md 列出导出头与消费的上游接口；`include/` 公开头未泄露 `src/`",
  "Debug / Release 双构建通过，ctest -R Social 全绿",
 ],
 forbid=[
  "禁止在 handler 中 switch 硬编码穷举社交类型（必须走注册表/枚举扩展）",
  "禁止直接改 Role 或 Scene 的私有数据（必须经 Command/接口）",
  "禁止世界频道用全服 O(N) 玩家遍历广播",
  "禁止为社交新建独立线程（沿用 Simulation 线程）",
  "禁止把 Guild/Mail 持久化逻辑写死在业务逻辑里（必须走 DataService）",
 ],
 perf="单 GameNode 社交操作吞吐 > 1k ops/s；Guild 双写额外延迟 < 1ms（异步）；世界频道单消息投递成本与接收者数量成正比、与全服人数无关。",
 deliver=["server/gamenode/social/include/mmo/game/social/social_system.h",
          "server/gamenode/social/src/*.cpp", "server/gamenode/social/tests/*",
          "config/gameplay/social.json", "server/gamenode/social/docs/INTERFACE.md"],
 ctest="Social", both_build=True,
 bench_bins=[], metrics=[], artifacts=["server/gamenode/social/include/mmo/game/social/social_system.h"],
),
# ------------------------------------------------------------------ 040
dict(
 id="TASK-040", name="ControlService（控制面：节点管理/配置下发/健康/运维）", phase="Phase 9 · 运维控制面",
 objective="实现集群控制面 ControlService：**节点注册表权威同步**（与 TASK-010 NodeRegistry 对接）、"
            "**配置版本（Config Version）下发**、**健康检查与运维接口**、**Gateway 多实例注册**。它不拥有任何游戏状态，只做控制与协调。"
            "灰度发布与自动扩缩容列为 Phase 2（见 docs/rfc/）。这是补齐「四进程」中唯一缺失的实现。",
 deps="TASK-003, TASK-006, TASK-010",
 module="server/control",
 owner=OWNER,
 inp="TASK-003 ConfigManager（Config Snapshot + 版本）；TASK-006 gRPC（HealthService/EchoService 已定义）；TASK-010 NodeRegistry（节点注册/健康/路由缓存）",
 out="ControlService 进程（或 GameNode 内控制线程）+ 节点管理接口 + 配置下发通道 + 运维接口 + 文档",
 iface="""```proto
// control_service.proto（增量扩展 TASK-006 的 service 定义）
service ControlService {
  rpc RegisterNode(RegisterNodeReq)   returns (RegisterNodeResp);   // GameNode/Gateway 上线注册
  rpc Heartbeat(HeartbeatReq)         returns (HeartbeatResp);      // 健康维持，含负载（player_count/tick_p99）
  rpc PushConfig(PushConfigReq)       returns (PushConfigResp);     // 携带 config_version，节点比对后热加载
  rpc QueryTopology(QueryTopologyReq) returns (QueryTopologyResp);  // 当前节点/路由拓扑（运维用）
}
message RegisterNodeReq { string node_id; string role; string addr; uint32 capacity; }
message PushConfigReq   { uint64 config_version; bytes snapshot; }
```""",
 data="**控制面数据（非游戏状态）**\n\n"
      "- 节点表：`node_id → {role, addr, capacity, load, last_heartbeat, status}`，权威在 ControlService，Gateway/GameNode 持缓存。\n"
      "- 配置版本：`config_version` 单调递增；节点收到 PushConfig 后比对本地版本，落后则热加载（复用 TASK-003 的原子指针替换）。\n"
      "- 路由权威：TASK-010 说 Redis 路由权威归 DataService/ControlService，本任务把「节点上下线 → 路由缓存失效」的协调做掉。\n\n"
      "**Gateway 多实例**：Gateway 注册到节点表后，前置 LB（四层/Envoy）按 `capacity/load` 分流；本任务不实现 LB 本身，只提供注册与健康数据。",
 thread="ControlService 独立进程（或 GameNode 内控制线程），事件驱动；心跳处理无锁更新节点表（单写者模型）。不与游戏 Simulation 争锁。",
 hot="NO", io="YES（健康/配置落盘与下发）", rpc="YES（gRPC ControlService）", persist="YES（节点表/配置版本持久化，可选 Redis）",
 files=["server/control/include/mmo/control/", "server/control/src/", "server/control/tests/",
        "server/control/docs/INTERFACE.md", "config/control/"],
 steps=[
  "定义 control_service.proto（RegisterNode/Heartbeat/PushConfig/QueryTopology），并入 TASK-006 的 proto 目录",
  "实现节点注册表：GameNode/Gateway 启动注册、心跳维持负载、超时判定离线",
  "实现配置下发：监听 config_version，落后则拉取并热加载（复用 TASK-003 原子替换）",
  "实现拓扑查询与运维接口（节点列表/路由/容量），供排查与容量报告使用",
  "对接 TASK-010 NodeRegistry：节点上下线触发路由缓存失效协调",
  "Gateway 多实例注册：Gateway 注册到节点表，输出 `capacity/load` 供前置 LB 分流",
  "写 INTERFACE.md：导出接口 + 消费的上游（TASK-003/006/010）+ 灰度/自动扩缩容的 Phase 2 边界说明",
  "集成测试：节点上下线 → 路由缓存失效 → 新节点可承接；配置版本推进 → 节点热加载",
 ],
 unit="节点注册/心跳/超时/配置版本比对各分支正确；路由失效协调在节点离线后触发",
 integ="起一个 GameNode + ControlService：GameNode 注册→心跳→ControlService 可见；kill GameNode→超时→ControlService 标记离线→路由失效；推进 config_version→GameNode 热加载新配置",
 bench="无（控制面低频）；记录单 ControlService 可管理节点数基线（> 1000 节点心跳不超时）",
 fail="ControlService 崩溃：游戏节点降级为「用本地缓存路由继续服务」，不雪崩（明确记录降级行为）；配置下发失败：节点保留旧版本并告警，不中断服务",
 accept=[
  "节点注册/心跳/超时/配置下发/拓扑查询全部实现并集成测试通过",
  "节点上下线正确触发 TASK-010 路由缓存失效协调",
  "config_version 推进后节点热加载新配置（复用 TASK-003 原子替换，无锁）",
  "Gateway 多实例注册并输出 capacity/load（前置 LB 分流所需数据齐备）",
  "ControlService 崩溃时游戏节点降级不雪崩（集成测试断言）",
  "INTERFACE.md 列出导出接口与消费的上游接口；`include/` 未泄露 `src/`",
  "Debug / Release 双构建通过，ctest -R Control 全绿",
 ],
 forbid=[
  "禁止 ControlService 拥有任何游戏状态（玩家/Scene/Entity 等）",
  "禁止在 GameNode 内硬编码节点表（必须来自 ControlService 下发）",
  "禁止配置变更破坏旧版本兼容（必须带 config_version 且可回退）",
  "禁止把灰度发布/自动扩缩容塞进第一版（明确列为 Phase 2 RFC）",
 ],
 perf="单 ControlService 管理 > 1000 节点心跳不超时；配置下发延迟 < 1s（千节点）；路由失效协调在节点离线超时后 < 1s 内完成。",
 deliver=["server/control/include/mmo/control/control_service.h",
          "server/control/src/*.cpp", "server/control/tests/*",
          "protocol/proto/service/control_service.proto", "server/control/docs/INTERFACE.md"],
 ctest="Control", both_build=True,
 bench_bins=[], metrics=[], artifacts=["server/control/include/mmo/control/control_service.h"],
),
# ------------------------------------------------------------------ 041
dict(
 id="TASK-041", name="跨进程集成与战斗性能回归", phase="Phase 8 · 集成与回归",
 objective="补齐「跨进程全链路」与「Lua 落地后的战斗性能回归」两块验证缺口："
            "① 跑一次**真实 gRPC + Redis + MySQL** 的端到端演练（Login→Gateway→GameNode→Scene→战斗→经济→持久化）；"
            "② 在 TASK-033（Lua 玩法脚本，运行在 Combat Tick 内）落地后，**重跑 TASK-025 的 1k 战斗性能矩阵**，防止 Lua 拖垮 Tick。"
            "本任务不拥有任何服务状态，是纯验证方，禁止改动被验证模块的实现。",
 deps="TASK-025, TASK-030, TASK-033",
 module="tools/qa",
 owner=OWNER,
 inp="TASK-025 战斗性能矩阵定义（五场景×四规模）；TASK-030 账本对账（economy_audit.py）；TASK-033 Lua 脚本全集（boss/event/quest/skill/npc）",
 out="跨进程端到端测试套件 + 战斗性能回归报告（Lua 前后对比）+ 回归门禁脚本",
 iface="""```cpp
// 无新增业务接口；消费既有接口：
//  - TASK-025 combat_benchmark：重跑矩阵，输出 tick_p95_us/phase_p95_us
//  - TASK-030 economy_audit：对账，断言五场景故障下资金守恒
//  - TASK-005 协议：驱动真实 Bot 走完整链路
namespace mmo::qa {
class E2EScenario { public:
  core::Result<void> Run(const E2EConfig&);   // 启 Gateway+GameNode+DataService，跑全流程
  RegressionReport Report() const; };         // Lua 前后 Tick 对比
}
```""",
 data="**回归矩阵（必须全跑，禁止抽样）**\n\n"
      "| 项 | 内容 |\n|---|---|\n"
      "| 跨进程 E2E | Bot 真实走 Login→Gateway→GameNode→EnterScene→Attack→Loot→Trade→Persist；断言各阶段可观测 |\n"
      "| 账本对账 | 复用 TASK-030 economy_audit.py，五场景故障下资金守恒（不重复扣/发/复制） |\n"
      "| 战斗回归 | 在 TASK-033 Lua 全量加载后，重跑 TASK-025 的 1000 玩家 100% Combat 矩阵，对比 Lua 前基线 |\n\n"
      "**判定**：Lua 后 tick_p95_us 仍 ≤ 5000、tick_p99_us 仍 ≤ 8000；任一退化 > 5% 即判回归失败并记录瓶颈（通常是某 Lua 脚本耗时）。",
 thread="本任务测试进程独立于服务进程；可指定多机部署以模拟真实跨进程。",
 hot="YES（回归对象即战斗热路径）", io="YES（写报告）", rpc="YES（驱动真实 gRPC）", persist="YES（触发真实持久化）",
 files=["tools/qa/e2e/", "tools/qa/regression/", "tools/qa/docs/REGRESSION.md"],
 steps=[
  "搭建跨进程 E2E 编排：本地起 Gateway + GameNode + DataService（Redis+MySQL），用真实 Bot 走完整链路",
  "实现 E2E 断言：每阶段可观测（Session 七字段、Scene 创建、战斗伤害、掉落入库、交易对账）",
  "接入 TASK-030 economy_audit.py：五场景故障下资金守恒断言",
  "在 TASK-033 Lua 全量加载后，调用 TASK-025 的 combat_benchmark 重跑 1k 100% Combat 矩阵",
  "产出回归报告：Lua 前/后 tick_p95/p99 对比，退化 > 5% 标红并定位耗时 Lua 脚本",
  "写 REGRESSION.md：回归门禁定义（阈值、频率建议：每次 Lua/战斗改动后跑）",
  "把回归挂为可重复命令：`bash tools/qa/regression/run.sh`",
 ],
 unit="E2E 编排器能正确拉起/销毁服务进程；回归报告解析与阈值判定正确",
 integ="完整 E2E 一次跑通（含一次注入的 GameNode 崩溃，验证 TASK-037 重连链路在真实跨进程下可用）；账本对账五场景全过",
 bench="复用 TASK-025 combat_benchmark（Lua 后重跑），输出 tick_p95_us/tick_p99_us/phase_p95_us",
 fail="回归退化 > 5%：报告标红并定位 Lua 脚本，不静默通过；E2E 中某服务启动失败：明确报错并清理残留进程，不留下僵尸",
 accept=[
  "跨进程 E2E 一次完整跑通（Login→…→Persist），每阶段断言可观测",
  "TASK-030 economy_audit 五场景故障下资金守恒全过",
  "TASK-033 Lua 全量加载后重跑 TASK-025 1k 战斗矩阵：tick_p95 ≤ 5000、tick_p99 ≤ 8000，退化 < 5%",
  "回归报告 REGRESSION.md 含 Lua 前/后对比与瓶颈定位",
  "回归命令可重复执行，作为后续 Lua/战斗改动的常驻门禁",
  "Debug / Release 双构建通过，ctest -R QA_E2E 全绿",
 ],
 forbid=[
  "禁止在回归中改动被验证模块（TASK-025/030/033）的实现",
  "禁止抽样跑矩阵（必须 5 场景×4 规模 + Lua 前后两次）",
  "禁止在退化 > 5% 时静默通过",
  "禁止 E2E 残留僵尸进程（必须清理）",
 ],
 perf="E2E 单轮 < 5min（含启动）；Lua 后 1k 100% Combat：tick_p95 ≤ 5000us、tick_p99 ≤ 8000us，与 Lua 前基线退化 < 5%。",
 deliver=["tools/qa/e2e/e2e_runner.h", "tools/qa/regression/run.sh",
          "tools/qa/docs/REGRESSION.md", "bench/combat_regression_lua.txt"],
 ctest="QA_E2E", both_build=True,
 bench_bins=[("bin/combat_bench", "--matrix --lua-loaded --duration 60 --warmup 5 --out bench/combat_regression_lua.json")],
 metrics=[("bench/combat_regression_lua.txt", "tick_p95_us", "le", "5000"),
          ("bench/combat_regression_lua.txt", "tick_p99_us", "le", "8000")],
 artifacts=["tools/qa/docs/REGRESSION.md", "bench/combat_regression_lua.txt"],
),
]
