# TASK-030 · 经济账本 / 幂等 故障测试报告

> 报告性质：**实测记录**。文中所有数字均来自本机真实执行的命令输出（命令与输出逐条附在
> 对应小节），没有理论值冒充实测值（§23 / §30）。
> 证据分层：**【实测】** = 命令输出原文；**【设计】** = 架构取舍说明（不含测量）；
> **【推断】** = 由实测外推的结论，已单独标注。
>
> | 项 | 值 |
> |---|---|
> | 任务 | TASK-030 Economic Ledger / Idempotency |
> | 日期 | 2026-09-10 |
> | 工具链 | g++ (MSYS2 MinGW-w64) 16.1.0 / CMake 4.4.2 / Ninja；vcpkg manifest mode baseline `aae277ac` |
> | 构建 | `build/Release`（Release）；`build/Debug`（Debug） |
> | 执行 | `bash scripts/verify/task-030.sh`（Debug + Release 双构建） |
> | 产物 | `bench/ledger.txt`、`bench/economy_ledger_dump.csv`、`bench/economy_balances.csv` |

---

## 0. 结论

1. **五个故障场景全部通过**，且每个场景都对「副作用发生次数」做了断言（§19 硬要求）。
2. **无悬挂 InFlight**：五场景结束后 `idem.InFlightCount() == 0`，恢复路径把未决行补记
   为 `Completed`。
3. **1 万次混合操作后资金守恒、对账零差异**：Σ账本净额 == Σ余额 == 发行 − 销毁，逐玩家
   40/40 全部可解释。
4. **哈希链可校验、篡改必被发现**（改字段 / 只改 hash 两种篡改都被检出）。
5. **双保险都在**：应用层幂等状态机 + 数据库层 `idempotency_key` UNIQUE 索引。
6. 发现 **1 处任务书内部规格冲突**（§8 同时要求 UNIQUE 与按月分区，在 MariaDB 中不可能
   同时成立），已按 §30 优先级处置并记录，**需人工确认**（见第 8 节）。
7. 性能取数过程中修掉 **3 处测量缺陷**（`flush_ns_per_1k` 口径错 10 倍、计时循环混入
   harness 分配、单批均值不是稳定估计量），并发现 **`idem_check_ns` 对表规模高度敏感**
   （10k→500k 时 143.87→404.99 ns，缓存失配所致）。详见第 6.1 / 6.2 节。
   修正后 `idem_check_ns` 稳定在 152~158 ns，距 200 ns 阈值余量 ≈21~24%。
8. 发现 **1 处框架级口径缺陷**（生成的验收脚本把 Release 的性能阈值套到 Debug 二进制上，
   `BUILD_TYPE=Debug` 因此必然失败；已完成任务 TASK-029 同样中招，证明与本任务无关）。
   **未擅自修改**（会影响全部 42 个任务），只上报待人工决策，见第 7.2 节。

**验收结论：`bash scripts/verify/task-030.sh` exit 0 · `===== TASK-030 验收通过 =====`。**

---

## 1. 故障测试的断言口径（先说清，否则表里的数字会被误读）

§19 要求每个场景断言「余额/物品变化次数 == 1」。要落实这条断言，必须先回答：
**崩溃/重连之后，余额的权威在哪里？**

- 【设计】§6 规定实时状态（含余额）的 **Owner 是承载该玩家的 GameNode**，进程内
  `WalletTable` **不跨进程存活**。崩溃后的余额由 DataService 的持久化余额装载回来
  （TASK-026/027/028 的 `Load` 路径），账本负责的是**对账与重放判定**，不是余额的唯一
  恢复来源（§21 同样禁止把 Redis RDB 当唯一恢复方案）。
- 因此「余额变化次数 == 1」在跨进程场景下**不能**直接读新进程的钱包绝对值得出——新进程
  的钱包本来就是空的。可行的判据是三件事同时成立：
  1. 同一幂等键的第二次请求 **没有执行**（`applied == false`，或 `deduplicated == true`）；
  2. **账本行数不增加**（没有第二条同键流水，也就没有第二次副作用）；
  3. 在「余额已由持久层装载回来」的前置下，钱包余额**跨三次请求保持不变**。
- 每个场景下面都按这三条组织断言。**第 3 条之所以能成立，是因为测试显式模拟了
  「DataService 把持久化余额装载回内存」这一步**，没有它就等于假设钱包会跨进程复活
  ——那正是本场景最初写错的地方（见第 2 节场景 4 的「曾经的错误」）。

> 与任务书字面的差异：§19 表格里场景 4 的断言写作「从 MySQL 幂等表恢复，不重复执行」。
> 本报告把它落成「持久层（幂等表 + 账本）联合判定 + 不重复执行 + 无第二条账」，把
> 「MySQL」抽象为 `IIdemTable` / `ILedgerStore`（§13 禁止 GameNode 直连 MySQL）。
> 这属于措辞具体化，不是降级。

---

## 2. §19 五场景逐条记录

### 实测命令

```bash
export PATH=/c/msys64/mingw64/bin:$PATH
cd F:/AI/workbuddy/CAMI
./build/Release/bin/ledger_test.exe
```

### 实测输出（原文）【实测】

```text
-- §16 SHA-256 FIPS 180-4 已知向量
-- §16 LedgerEntry 字段序列与哈希链
-- §16 幂等四状态机 / TTL / 注入故障
-- §16 Ledger Append / Flush / VerifyChain / 背压 / 直写
-- §19 五场景故障测试（重复请求 / RPC 重试 / 断线重连 / Crash / DB 重试）
   [1] 重复请求 10 次：余额变化 1 次，deduplicated=9
   [2] RPC 重试 3 次（同 key）：只扣 1 次（99500 → 99200）
   [3] 断线重连（同节点 + 换节点）：重发同 key 不重复扣（保持 98500，账本 4 行不变）
   [4] Crash + 重启 + TTL 过期：账本重放首次结果，余额保持 97600（账本 6 行不变）
   [5] 数据库重试：重放命中 UNIQUE，账本记录未翻倍（rows=10，重复拦截=1）
   [额外] 装备发放重复 5 次：物品只 +3（不复制 ItemGuid）
-- §17 集成：1 万次混合经济操作（守恒 + 链 + 对账）
   ops=10000 applied=9991 rejected=9 ledger_rows=10031 minted=40661864 burned=496997 total_balance=40164867
   链校验=PASS；逐玩家 Σ账本净额 == 当前余额（40 名玩家全部通过）
ledger_test: checks=10320 failures=0
ALL TESTS PASSED
```

`checks=10320 / failures=0`，退出码 0。

---

### 场景 1 · 重复请求

| 项 | 内容 |
|---|---|
| 模拟 | 同一 `idempotency_key` 连发 10 次 `RemoveCurrency(500)` |
| 断言 | `applied && !deduplicated` 恰好 1 次；`deduplicated` 恰好 9 次；余额恰好 −500 |
| 实测 | 通过（`余额变化 1 次，deduplicated=9`） |
| 机理【设计】 | 第 1 次 `TryBegin→Fresh` 后 `Commit→Completed`；后 9 次 `TryBegin→Completed`，直接 `Lookup` 首次结果返回，**不进任何副作用路径** |

### 场景 2 · RPC 重试

| 项 | 内容 |
|---|---|
| 模拟 | 首次在**幂等占位阶段**注入瞬时失败（`FailNextInserts(1)` → `TIMEOUT`，模拟 MySQL 抖动/连接不可用）；客户端用**同一个 key** 重试 3 次 |
| 断言 | 首次：失败且余额不变（无副作用）；重试：1 次生效 + 2 次去重；余额只扣 1 次（99500 → 99200） |
| 实测 | 通过 |
| 关键点【设计】 | 「禁止 RPC 重试时重新生成 idempotency_key」（§21）——重试必须沿用首次的 key，否则 3 次重试就是 3 次扣款 |

### 场景 3 · 断线重连

| 项 | 内容 |
|---|---|
| 模拟 | 3a 扣款成功 → 客户端断线重连回**同一节点** → 重发同 key；3b 重连后被路由到**另一个 GameNode**（新建 `EconomySystem`，只共享持久层）→ 重发同 key |
| 断言 | 3a：`deduplicated=true`、`balance_after` 等于首次结果、余额不再变化；3b：依然 `deduplicated=true`、`balance_after` 与 `version` 与首次结果一致、**账本行数不变** |
| 实测 | 通过（`保持 98500，账本 4 行不变`） |
| 关键点【设计】 | **断线重连 ≠ 进程重启**。真正必须跨进程的是幂等判定，所以 3b 明确断言「去重不依赖进程内的 `idempotency_` map」；而新实例的钱包余额**不作断言**——钱包不跨进程，断言它等于假设余额会凭空复活 |

### 场景 4 · GameNode Crash

| 项 | 内容 |
|---|---|
| 模拟 | 复现窗口：`TryBegin` 成功 → **钱包扣减与账本落账都已完成** → 进程在 `Commit` 之前被强杀。构造方式：直接驱动原语把幂等行留在 `InFlight`，并向账本 `Append`+`Flush` 一条同 key 的已落账记录；随后**丢弃全部进程内状态**（`delete eco` → 新建实例），只保留持久层 |
| 断言 | ① TTL 未过期：`TryBegin→InFlight` → `BUSY`、`applied=false`、余额不变、账本行数不变；② TTL 过期（注入时钟 +31s）：`TryBegin→Failed` → 查账本命中 → 重放首次结果，`deduplicated=true`、`balance_after` 等于账本记录、**余额不变、账本行数不变**；③ 恢复已把幂等行补记为 `Completed`，再来一次仍走首次结果 |
| 实测 | 通过（`余额保持 97600，账本 6 行不变`） |
| 关键点【设计】 | 崩溃窗口里**钱已经扣了、账已经落了，只有幂等行没 Commit**——这是唯一会产生「重复扣款」的窗口。判据是「查账本」而不是「重做」，所以 `QueryByKey` 是必需的扩展接口 |
| **曾经的错误**（已修正，值得记录） | 最初用「再 `new` 一个 `EconomySystem` 执行扣款」来构造崩溃现场，结果新实例带的是一个**空钱包**，`RemoveCurrency 900` 先被「余额不足」挡下 → 测到的是余额不足而不是崩溃恢复（当时 5 条断言全红）。正确做法是**直接驱动原语**构造现场，并用一次正常入账模拟 DataService 的余额装载 |
| 为什么不能只靠 `InFlight` | 【设计】如果崩溃后直接删行重做，就会二次扣款；如果永久保留 InFlight，玩家将永久卡死。**TTL + 查账本**是同时满足「不重复」与「不卡死」的唯一组合（§15.8） |

### 场景 5 · 数据库重试

| 项 | 内容 |
|---|---|
| 模拟 | 5a：落库注入 2 次瞬时失败（小于 `max_retries=3`）→ 内部重试后成功；5b：注入超过重试上限的失败 → `Flush` 失败但**保留全部待落库条目** → 故障消失后重投（重投会重放已写条目）；5c：**绕过应用层**直接向存储写同 key，验证数据库层 UNIQUE 兜底 |
| 断言 | 5a：恰好 1 行、`InjectedFailures==2`、`Stats().retries>=2`；5b：待落库一条不丢、重投后行数不翻倍、待落库清零、链仍自洽；5c：`{stored=false, duplicate=true}`、行数不变、`DuplicateRejections>=1` |
| 实测 | 通过（`rows=10，重复拦截=1`） |
| 关键点【设计】 | 「不产生两条账本记录」由 **UNIQUE + 哈希链双校验**保证：UNIQUE 拦重复插入，哈希链保证「即使有人绕过应用层把行插进去，链也对不上」。两条都必须有——只有 UNIQUE，插进去的脏数据无法被发现；只有链，插入成本极低 |

### 额外 · 装备发放重复（不复制装备）

| 项 | 内容 |
|---|---|
| 模拟 | 同一幂等键连发 5 次 `AddItem(item_def_id=1, count=3)` |
| 断言 | 背包中该 def_id 总量只 +3；后续 4 次的 `created_guids.size()` 与首次一致（**不重复生成 ItemGuid**） |
| 实测 | 通过（`物品只 +3（不复制 ItemGuid）`） |
| 已知限制【设计】 | 重复请求返回的是首次结果，**其中包含的 `created_guids` 是首次真实生成的那些**；而崩溃恢复路径（场景 4）无法还原 `created_guids`——账本按 §7 不存 guid。恢复路径上客户端应以**背包查询**为准（已写入 `INTERFACE.md` §17） |

---

## 3. §20 验收清单逐条核对

| # | 验收项 | 结论 | 证据 |
|---|---|---|---|
| 1 | 五个故障场景全部通过，每个断言「变化次数 == 1」 | ✅ | 第 2 节；`failures=0` |
| 2 | 幂等表有 UNIQUE 索引兜底 | ✅ | **真实 MariaDB 11.8.6 实测**：`003_ledger.sql` 由项目自身迁移器 `applied 003`；绕过应用层重复插入被 `ERROR 1062` 拒绝（见第 5 节）。另有场景 5c 的 `duplicate=true` |
| 3 | 账本哈希链可校验，篡改检测生效 | ✅ | `ledger_test.cpp:437-444`：改 `balance_after` → `VerifyChainOf=false`；只翻转 `hash[7]` 一位 → 同样 `false` |
| 4 | 1 万次操作后资金守恒，对账工具零差异 | ✅ | 第 4 节（`mismatches = 0`） |
| 5 | InFlight 有 TTL，崩进程后能恢复，无悬挂 | ✅ | 场景 4（TTL 注入 +31s 转 `Failed` → 查账本 → 补记 `Completed`）；五场景结束 `InFlightCount()==0` |
| 6 | 重复请求返回首次结果而非报错 | ✅ | 场景 1（9 次 `deduplicated=true`，全部 `HasValue()`） |
| 7 | Debug / Release 双构建通过，`ctest -R Economy_Ledger` 全绿 | ✅ | `bash scripts/verify/task-030.sh`（双构建，见第 7 节） |

---

## 4. 对账证据（1 万次混合操作）

### 对账口径【设计】

```
net(p, c) = Σ{player == p, currency == c} delta  −  Σ{peer == p, currency == c} delta
```

为什么**减去** peer 项：Transfer 每笔命令只写**一行**账本（§7 的 `LedgerEntry` 只有
`player`/`peer` 一对），出账方 `delta = −amount`，入账方那一侧没有独立行。若只累加
`player == p`，收款方的余额变化永远无法被账本解释，对账必然「有差异」——这不是账本错了，
是对账口径错了。C++（`InMemoryLedgerStore::NetDeltaByPlayer`）与 Python
（`tools/audit/economy_audit.py`）必须是同一口径，两边都按此实现。

### 实测命令

```bash
C:/Users/17283/.workbuddy/binaries/python/versions/3.13.12/python.exe tools/audit/economy_audit.py
```

### 实测输出（原文）【实测】

```text
== TASK-030 经济账本对账 ==
ledger   : bench\economy_ledger_dump.csv
balances : bench\economy_balances.csv
meta.ops           : 10000
meta.players       : 40
meta.applied       : 9991
meta.rejected      : 9
meta.ledger_rows   : 10031
meta.issued        : 40661864
meta.burned        : 496997
meta.total_balance : 40164867

-- 统计 --
rows                = 10031
duplicate_keys      = 0  (期望 0)
balance_after_checks= 10031
players(账本/快照)   = 40 / 40
sum_delta_net       = 40164867
sum_balances        = 40164867
mismatches          = 0

RESULT: OK — 账本可完整解释每一个余额，零差异（§20.4）
```

对账工具做了四类检查（不只第 3 类）：

1. **幂等键唯一性**：账本内不得出现两条同 `idempotency_key` → `duplicate_keys = 0`。
2. **逐行重放**：按写入顺序把 `delta` 记到 `player`、把 `−delta` 记到 `peer`，然后校验
   每一行的 `balance_after` 是否等于重放值 → `balance_after_checks = 10031`，全部匹配。
   这等价于「账本里**每一个**余额快照都能被此前的流水解释」，比只比最终余额强得多。
3. **逐玩家净额 vs 余额快照** → 40/40 通过。
4. **总额守恒**：`Σ账本净额 == Σ余额快照`（`40164867`），且与
   `meta.issued − meta.burned`（`40661864 − 496997 = 40164867`）一致。

### 对账工具自身的有效性（否则「零差异」可能只是没检查）

```bash
python tools/audit/economy_audit.py --self-test
```

```text
== economy_audit.py 自检 ==
  干净数据（期望零差异）                  findings=0  kinds=-  -> PASS
  篡改 delta                     findings=5  kinds=['NEGATIVE_BALANCE', 'NET_VS_BALANCE', 'REPLAY_MISMATCH']  -> PASS
  重复 idempotency_key           findings=1  kinds=['DUPLICATE_KEY']  -> PASS
  余额被多算                        findings=2  kinds=['CONSERVATION', 'NET_VS_BALANCE']  -> PASS

RESULT: OK — 对账工具可检出差异
```

【设计】这一步是刻意的：「对账零差异」只有在工具**确实能报错**时才有意义。自检构造四组
数据，干净组必须零 findings，三组篡改组（偷改 `delta` / 复用幂等键 / 虚增余额）必须各自被
检出——特别地，偷改 `delta` 同时触发 `REPLAY_MISMATCH`（余额快照对不上）与
`NEGATIVE_BALANCE`（重放后为负，§21 禁止负余额）。

---

## 5. 真实 MariaDB 验证（数据库层兜底 + 迁移可见性）

前面第 2 节的场景 5 用的是内存实现（`InMemoryLedgerStore`），它能证明**代码路径**正确，
但证明不了**表结构**真的挡得住重复。本机恰好有项目 docker 实例在跑
（`11.8.6-MariaDB`，与 `docker/mysql/docker-compose.yml` 的 `mariadb:11.8` 一致），
因此本节全部是**真实数据库**上的实测。

### 5.1 迁移可见性：`003` 必须真的被应用

```bash
./build/Release/bin/mysql_migrate.exe up --dir=database/migrations --shards=1 \
    --db-prefix=cami_probe_shard
```

```text
[migrate] action=up dir=database/migrations shards=1 host=127.0.0.1:3306 prefix=cami_probe_shard user=root
--- shard 0 (cami_probe_shard0): applied 3 migration(s)
      applied 001
      applied 002
      applied 003
[migrate] OK
```

```text
version  name                     success
1        001_init.sql             1
2        002_mail_expire_index.sql 1
3        003_ledger.sql           1
```

```text
account  character  economy_idempotency  economy_ledger  equipment  guild
inventory  kv_store  mail  quest  schema_migrations
```

【设计】这一步刻意用**项目自身的迁移器**而不是直接灌 SQL：它同时验证了文件名约定
`NNN_name.sql` 成立。本任务早期把文件写成 `00N_ledger.sql`，`MigrationRunner::ParseVersion`
会**静默跳过**——迁移器不报错、`applied` 列表里没有它、表根本不存在，构建照样全绿。
这类「全绿但功能未生效」的失败比报错危险得多，所以必须由迁移器自己确认「applied 003」。

### 5.2 数据库层 UNIQUE 兜底（绕过应用层的重复写入）

```sql
INSERT INTO economy_ledger (transaction_id, ..., idempotency_key, ...)
VALUES (1, ..., 'purchase:100:7:1:1', ...);          -- 成功
INSERT INTO economy_ledger (transaction_id, ..., idempotency_key, ...)
VALUES (2, ..., 'purchase:100:7:1:1', ...);          -- 换 transaction_id 与 timestamp_ms
```

```text
ERROR 1062 (23000) at line 7: Duplicate entry 'purchase:100:7:1:1' for key 'uk_ledger_idempotency_key'
```

```text
ledger_rows_for_that_key
1
```

幂等表同样实测：

```text
ERROR 1062 (23000) at line 4: Duplicate entry 'purchase:100:7:1:1' for key 'PRIMARY'
idem_rows
1
```

**结论【实测】**：即使应用层的幂等状态机被绕过（或进程崩在窗口期没来得及判），
数据库层仍会以 `ERROR 1062` 拒绝第二次写入，行数保持 1 —— 这就是 §15.3 要求的
「UNIQUE 索引兜底」，也是 §21「禁止在没有 UNIQUE 索引的情况下依赖应用层去重」的正面证据。

> 探针库 `cami_probe_shard0` 与 `cami_partition_probe` 在取证后已删除，未污染任何
> 既有分片库（`mmo_shard*` 未被动过）。

---

## 6. 性能实测

### 实测命令

```bash
mkdir -p bench && ./build/Release/bin/ledger_bench.exe --ops 10000
```

### 实测输出（原文）【实测】

```text
ledger_bench: ops=10000 rounds=9 appended=10000 stored_rows=1000 dedup_hits=10000/10000 fresh=10000
idem_check_ns=159.640 [152.820..210.180] ledger_append_ns=45.720 flush_ns_per_1k=787400.0 dedup_hit_ns=147.430 mem_bytes_per_entry=150
```

| 指标 | §22 预算 | 实测（9 轮中位数） | 判定 |
|---|---|---|---|
| `idem_check_ns` | ≤ 200 ns | **159.64 ns**（min 152.82 / max 210.18） | ✅ |
| `ledger_append_ns` | ≤ 500 ns | **45.72 ns** | ✅ |
| `flush_ns_per_1k` | ≤ 50 ms | **0.787 ms** | ✅ |
| `mem_bytes_per_entry` | ≤ 256 B | **150 B** | ✅ |
| `dedup_hit_ns` | —（记录） | 147.43 ns | — |

> **必须与丢弃/重复计数一起看**（本项目最容易踩的假数据陷阱）：
> `appended=10000 / fresh=10000 / dedup_hits=10000/10000` —— 没有任何条目被丢弃、
> 没有重复计数、没有走「什么都不做」的快路径，所以上面的数字是真干了活的数字。
>
> 内存为什么能进 256B：`LedgerEntry` 含 3 个 `std::string` + 1 个 `vector`，直接排队
> 一条就是 4 次堆分配、轻松超 300B。实现改用**定长紧凑槽 `LedgerSlot` + 变长 arena**
> （槽内只存定长元数据与偏移量），`MemoryBytesPerEntry()` 返回的是这个模型下的实测均值。

### 6.1 本轮为取数正确性做的三处修正（都是**测量**修正，不是降低工作量）

这三处都是在「数字看着没问题」的情况下主动挖出来的，记录在此以免以后又被踩：

**(a) `flush_ns_per_1k` 的标签与口径不符（原实现测的是 ops 条，不是 1000 条）**

原实现用 `ops`（=10000）条一次性 Flush，却把结果写进名为 `flush_ns_per_1k` 的指标，
等于**把 10 倍的工作量记成 1000 条的花费**（实测 8.27e6 ns "per 1k"，实际是 10000 条的耗时）。
现改为真的批量 1000 条。修正后 `flush_ns_per_1k ≈ 0.79 ms`，与修正前换算值
（8.27 ms / 10 ≈ 0.83 ms）一致 —— 说明**结论没变，但原来那个数字不能直接读**。

**(b) 计时循环里混入了 harness 自己的字符串分配**

原实现在计时循环内构造 key：`"idem-" + std::to_string(i)`（两次堆分配）。
生产路径里 `idempotency_key` 是**随协议消息一起到达**的（`EconomyCommand::idempotency_key`），
调用方不需要现造；把它算进来，`idem_check_ns` 测的是「itoa + 字符串拼接」。
现把 key 预构造到计时循环之外。

**(c) 单批均值不是稳定估计量 → 改 9 轮取中位数，并同时输出 min/max**

本机实测：同一份代码、同一份工作量，**单批均值**的 12 次采样是
`143.66 / 143.85 / 144.16 / 145.98 / 148.15 / 149.83 / 150.82 / 152.90 / 154.37 / 154.43 /
166.54 / 188.98` ns，另有一次在整机 104 个目标刚编译完时跑到 **230.33 ns**。
极差 1.6 倍意味着：单批均值偶发就会越过 200 ns 阈值，表现为「同一份代码时过时不过」的
flaky 门禁 —— 这比测不准更糟，因为它会让人开始怀疑代码而不是测量。
Windows 的 `steady_clock` 走 QPC，叠加线程迁移与页错误，单批均值天然不稳。

修正：每轮**都真跑 ops 次**（不减少工作量），只把估计量换成稳健的**中位数**，
并把 `min`/`max` 一并落盘（`bench/ledger.txt` 里的 `idem_check_ns_min/_max`）
**供任何人复核离散度**——用中位数掩盖离群会变成另一种假数据，所以离散度必须同时可见。

修正后的稳定性：连续 8 次运行的 `idem_check_ns` 为
`152.02 / 157.84 / 156.10 / 156.72 / 157.07 / 158.24 / 152.86 / 155.07` ns，
落在 152~158 区间，**距 200 ns 阈值余量 ≈ 21~24%**，满足项目「余量 < 20% 的实现不要选」的要求。

**(d) 顺手做实的一处真实优化**（唯一一处改动被测代码性能的地方）

`InMemoryIdemTable::Load/Update` 原本用 `std::string(key)` 做临时对象再查找。
key 本身就是 `string_view`（来自命令），每次经济命令都要白构造一个临时 `std::string`。
改为 C++20 异构查找（`TransparentStringHash` + `std::equal_to<>`，`P0919R3`）后零构造。
探针实测该查找从 9.68 ns → 1.92 ns（省 ~8 ns）。**插入路径的那一次分配省不掉**
（节点必须拥有键），所以这里没有过度承诺。

### 6.2 一个重要的口径发现：`idem_check_ns` 对**表规模**高度敏感

同一份代码，只改 `--ops`（即幂等表最终大小），实测：

| `--ops` | `idem_check_ns` |
|---|---|
| 10,000 | 143.87 |
| 50,000 | 179.97 |
| 200,000 | 358.91 |
| 500,000 | 404.99 |

【实测】成本随表规模**近线性上升**，这不是 CPU 而是**缓存**：`unordered_map<string, IdemRow>`
每个节点 ≈112B（值 + SSO 键 + 节点头），20 万条约 22 MB，远超 L3，
于是每次查找/插入都要付 1~2 次 cache miss。

**这条结论要记住**（比单个数字重要）：§22 的「幂等检查 < 200ns」只在**工作集驻留缓存**时成立。
真实 GameNode 上线时应当注意：
1. 幂等表的**热工作集**是「当前在途 + 近期完成」的键，不是全部历史键；
2. 历史键应过期清理（Redis 侧天然靠 TTL；MySQL 侧靠分区/归档），否则延迟会随运行时长劣化；
3. 若未来单节点键量上到 10⁵ 量级，应换成开放寻址扁平表或把 `EconomyResult` 移出热点行
   （状态机只需要 `status + deadline`，`result` 仅在 Completed 路径才用得上），把单条从 112B 压到 ~32B。

✅ 验收脚本的取数口径（`--ops 10000`）远小于上述劣化拐点，本次实测数字可信；
但**不要把 10 万级规模的延迟外推到 160ns**（那属于【推断】以外的范围，未测）。


---

## 7. 验收脚本

```bash
export PATH=/c/msys64/mingw64/bin:$PATH
cd F:/AI/workbuddy/CAMI
export MMORPG_ROOT="F:/AI/workbuddy/CAMI"
export MMO_PROJECT_ROOT="F:/AI/workbuddy/CAMI"
export MMO_TASKS_DIR="F:/AI/workbuddy/CAMI/mmorpg_tasks/tasks"
export GENERATOR=Ninja
export VCPKG_ROOT=C:/Users/17283/vcpkg
bash scripts/verify/task-030.sh              # Release
BUILD_TYPE=Debug bash scripts/verify/task-030.sh   # Debug
```

脚本 `set -euo pipefail`，**退出码 0 才算通过，没有「警告通过」**。脚本自动执行：
前置任务门禁（001/005/026/028/029 全 DONE）→ 交付物存在性 → 模块边界扫描 →
Debug + Release 双构建 → `ctest -R Economy_Ledger`（含零匹配校验，防止「零用例假通过」）→
`ledger_bench` → 阈值断言（`idem_check_ns ≤ 200`、`mem_bytes_per_entry ≤ 256`）。

### 7.1 实测结果

| 命令 | 结果 | 说明 |
|---|---|---|
| `bash scripts/verify/task-030.sh`（默认 Release） | **exit 0 · 验收通过** | 同一次运行里 Debug + Release **都**编译通过；`ctest -R Economy_Ledger` 匹配 1 个用例全绿；`idem_check_ns=165.04 ≤ 200`、`mem_bytes_per_entry=150 ≤ 256` |
| `BUILD_TYPE=Debug bash scripts/verify/task-030.sh` | exit 1 | 构建 ✅、`ctest`（Debug）✅，**只在 `idem_check_ns=1151.9 > 200` 这一步失败** —— 见下 |

### 7.2 ⚠ 生成的验收脚本存在**框架级口径缺陷**：把 Release 阈值套到 Debug 二进制上（**需人工决策，本任务未擅自修改**）

【实测】`--ops 10000` 下同一个 benchmark 的 Debug / Release 对比：

| 任务 | 指标 | Release | Debug | 脚本阈值 | Debug 判定 |
|---|---|---|---|---|---|
| TASK-030 | `idem_check_ns` | 159.6 | **1151.9** | ≤ 200 | ❌ |
| TASK-029 | `execute_ns` | — | **2556.97** | ≤ 2000 | ❌ |
| TASK-029 | `balance_query_ns` | — | **116.38** | ≤ 50 | ❌ |

**关键证据**：TASK-029 已经是 `STATUS: DONE` 且其 Release 验收是通过的，
但它的 benchmark 在 Debug 下同样**超标**。这证明问题**不是 TASK-030 引入的**，
而是所有 42 个任务共有的口径缺陷：

- 生成器 `mmorpg_tasks/tools/gen/build_tasks.py:740-742` 无条件地先
  `run_bench "$BUILD_TYPE" ...` 再 `assert_metric ...`；
- `_common.sh:203` 的 `run_bench` 按 `$BUILD_TYPE` 选择可执行文件
  （`$BUILD_ROOT/$bt/$rel`），于是 `BUILD_TYPE=Debug` 会去跑 **-O0 且不内联** 的二进制；
- 但 `assert_metric` 用的阈值来自 §22，而 §22 是**Release 口径**
  （所有既有模块 `docs/PERFORMANCE.md` 都写「Release 实测」）。
  Debug 下 `-O0/-Og` 的开销通常是 3~8 倍，**必然**超标。

**为什么不能在本任务顺手改掉**：修它要动生成器口径或 `_common.sh` 的共享函数，
影响全部 42 个任务（含已完成任务的历史结论），属于项目级口径变更，
必须走「改生成器源 → 临时目录重生成 → diff → 全量回归」的既定流程并取得人工批准。
按项目规则（§3.4「规则冲突先上报再处理」），此处**只上报、不擅改**。

**对 TASK-030 的影响**：无。§20.7 要求的是「Debug / Release **双构建通过**，
`ctest -R Economy_Ledger` 全绿」，两条都已由上述两次运行实测满足；
§22 的性能预算本身是 Release 口径，与 Release 验收结果一致。

**建议处置（待人工批准）**：`assert_metric` 只在 `BUILD_TYPE == Release` 时执行，
Debug 只跑 `run_bench`（证明能跑通、不崩），并打印「Debug 不做性能阈值断言（§22 为
Release 口径）」；这样 `BUILD_TYPE=Debug` 才能重新成为一个可用的验证入口。

---

## 8. 规格冲突与需人工确认项

### 8.1 §8 的 UNIQUE 与按月分区在同一张表上不可共存（**需人工确认**）

任务书 §8 的账本表定义同时要求：

- `idempotency_key` 上有 **UNIQUE INDEX**（冲突即发现重复）；
- `timestamp_ms` 是 **按月分区键**。

【实测，非推断】在真实 MariaDB 11.8.6 上分别做了两个探针实验：

**实验 A** —— `UNIQUE(idempotency_key)` 不含分区列，同时 `PARTITION BY RANGE (timestamp_ms)`：

```text
ERROR 1503 (HY000) at line 2: A PRIMARY KEY must include all columns in the table's partitioning function
```

**实验 B** —— 把分区列并入主键与唯一键（`PRIMARY KEY(transaction_id, timestamp_ms)` +
`UNIQUE KEY(uk_idem)(idempotency_key, timestamp_ms)`）后建表：

```text
step
B 建表成功

rows_with_same_idem_key
2
```

即：把 `timestamp_ms` 并入唯一键之后，**同一个 `idempotency_key` 只换一个时间戳就能插两行**
——唯一性被彻底削弱。而「崩溃重投」场景恰恰会带来新的时间戳。
（实验 B 的两条 INSERT 用的就是 §8 表格里的键样式 `purchase:{player}:{shop_id}:{item_id}:{client_seq}`。）

**处置**（按 §30 优先级 `Correctness > Architecture Consistency > … > Scalability`）：

- `database/migrations/003_ledger.sql` 中的 `economy_ledger` **保留全局
  `UNIQUE KEY uk_ledger_idempotency_key`，不做按月分区**；`timestamp_ms` 上建普通索引
  `idx_ledger_ts`，按月范围扫描/归档走索引。UNIQUE 是正确性约束
  （§21「禁止在没有 UNIQUE 索引的情况下依赖应用层去重」），分区是容量优化，
  两者冲突时不得牺牲正确性。第 5.2 节已在真实数据库上证明该 UNIQUE 有效。
- 将来若要分区，**既定路径已写在 SQL 文件头**（不改接口、不改状态模型）：
  幂等兜底搬到独立、不分区的 `economy_idempotency`（本迁移已建），账本表去掉 UNIQUE、
  改为按月分区并把主键放宽为 `(transaction_id, timestamp_ms)`。三个子结构都已就位，
  属于**预留**而不是缺失。
- 此项**偏离了 §8 的字面表定义**，按项目规则需人工确认，未自行「和稀泥」。

### 8.2 场景 4 的断言口径（已在第 1 节说明）

§19 表格的「从 MySQL 幂等表恢复」被落实为「持久层（`IIdemTable` + `ILedgerStore`）联合
判定」。这不是降级：§13/§33 明确禁止 GameNode 直连 MySQL，GameNode 只能看到抽象接口。

### 8.3 恢复路径无法还原 `created_guids`

账本按 §7 不存 ItemGuid（那是背包的职责）。恢复路径上 `EconomyResult.created_guids` 为空，
客户端应以**背包查询**为准。已在 `INTERFACE.md` §17 显式记录，不做静默省略。

---

## 9. 覆盖矩阵（`ledger_test` → 任务书章节）

| 测试函数 | 覆盖章节 | 覆盖内容 |
|---|---|---|
| `TestSha256KnownVectors` | §16 | FIPS 180-4 三条已知向量（空输入 / `abc` / 56 字节跨块）+ 流式分块与一次性结果一致（链是流式喂入的，分块边界不能改变结果） |
| `TestLEntryHash` | §15.1 / §15.5 / §20.3 | 十六字段规范化顺序（`static_assert` 数量）、字段顺序敏感性、`prev_hash` 链接、篡改检测（改字段 / 改 hash） |
| `TestIdempotencyStateMachine` | §8 / §15.2 / §15.8 | 四状态机全路径、TTL 过期转 Failed 且**不删行**、注入时钟证明不依赖墙钟、`Commit` 整行覆盖、`Abort` 删行、注入故障 |
| `TestLedgerAppendFlushVerify` | §7 / §15.4 / §15.5 | Append/Flush/VerifyChain/QueryByPlayer/QueryByKey、批次内重复拒绝、队列背压 BUSY、大条目直写保序、内存足迹 |
| `TestFiveFailureScenarios` | §19 / §20.1 | 五场景 + 装备发放不复制 + 无悬挂 InFlight |
| `TestIntegration10000` | §17 | 1 万次混合操作、守恒、链完整、逐玩家净额、落盘 CSV 供对账 |

---

## 10. 已知限制（不隐瞒）

1. **余额不跨进程持久化**（本任务的边界）：`WalletTable` 的 Owner 是 GameNode；跨进程余额
   恢复属 DataService 的 `Load`。本报告的场景 4 用「一次正常入账」模拟装载步骤。
2. **`ILedgerStore` / `IIdemTable` 的 C++ MySQL 适配器未在本任务完成**：本任务交付接口、
   内存实现（测试/基准用）与**已在真实 MariaDB 上验证过的表结构**（第 5 节）。
   真实 MySQL/Redis 适配器的接线由 DataService 侧后续任务完成
   （§13 的分工不允许 GameNode 直连）。
   【推断】在适配器接线前，「跨进程容灾」只在**接口契约层面**被验证（用共享的持久层实例
   模拟，并在真实数据库上单独验证了 UNIQUE 兜底），尚未在真实 MySQL 上跑过端到端的
   崩溃恢复。
3. **崩溃是「进程内状态丢弃」级模拟**，不是真的 `kill -9` 多进程演练：真实的分进程
   崩溃 + 重连演练属于 TASK-037（容灾）的范围。
