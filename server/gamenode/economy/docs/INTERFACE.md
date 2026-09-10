# TASK-029 · Economy System 接口说明

`mmo::game::economy`：货币与物品经济操作的**唯一**入口。所有余额/物品变更必须构造
`EconomyCommand` 并经 `EconomySystem::Execute` 执行；这是由类型系统强制的，不是约定。

## 1. 为什么绕不过 EconomyCommand（§20.1）

`Wallet`（货币容器）的写入方法 `Add` 是 `private`，`friend` 只给了 `EconomySystem`：

```cpp
struct Wallet {
    core::Result<std::int64_t> Get(CurrencyType) const noexcept;  // 只读，公开
private:
    friend class EconomySystem;
    core::Result<void> Add(CurrencyType, std::int64_t delta) noexcept;  // 唯一写入点
};
```

因此**任何**其它模块（含本模块的非 Execute 路径）在编译期就无法修改余额——
验收项「grep：无直接改余额的代码路径」由此升级为编译器保证，而非人工巡检。

物品侧同理：只经 `inventory::InventorySystem::Add/Remove`（§27.2），本模块不触碰背包内部。

## 2. 公开接口

```cpp
class EconomySystem {
public:
    explicit EconomySystem(inventory::InventorySystem& inv) noexcept;

    core::Result<EconomyResult> Execute(const EconomyCommand&, const SceneContext&);
    core::Result<std::int64_t>  Balance(PlayerId, CurrencyType) const noexcept;
    core::Result<void>          SetPriceTable(const PriceTable&);
    void                        SetLedgerSink(ILedgerSink*) noexcept;

    EconomyStats Stats() const noexcept;   // op / applied / dedup / fail / pending / minted / burned
    std::size_t  IdempotencySize() const noexcept;
    std::size_t  WalletCount() const noexcept;
    std::int64_t TotalBalance() const noexcept;   // Σ 全部玩家全部币种（守恒校验用）
};
```

### 八种操作（`EconomyOp`，§7 冻结）

| Op | 语义 | 记账 |
|---|---|---|
| `AddCurrency` | 发行货币给 player | `total_minted += amount` |
| `RemoveCurrency` | 从 player 销毁货币 | `total_burned += amount` |
| `AddItem` | 发放物品（`item_deltas.count > 0`） | — |
| `RemoveItem` | 收回物品（`item_deltas.count < 0`） | — |
| `Transfer` | 双人原子转移（货币和/或物品） | 不增不减（守恒） |
| `Purchase` | 查价格表 → 扣币 → 发物品 | `total_burned += 总价` |
| `Reward` | 任务/活动奖励（货币 + 物品） | `total_minted += amount` |
| `Refund` | Purchase 的逆（退币 + 物品按符号增/减） | `total_minted += amount` |

**货币守恒不变量**（§17）：`TotalBalance() == Stats().total_minted - Stats().total_burned`。
Transfer 只是搬家，不 mint 也不 burn，故不影响该等式。

### 必填字段（缺一即拒绝，§8）

| 字段 | 说明 |
|---|---|
| `transaction_id` | 全局唯一，写入账本 |
| `idempotency_key` | 幂等键；空 → 拒绝执行（§20.4 硬性校验） |
| `player` | 0 → 拒绝 |
| `reason` / `source` | 审计必需，空 → 拒绝 |

执行顺序：校验参数 → 查幂等表 → 检查余额/背包 → 扣/加 → 异步投递账本 → 发布事件 → 返回。

## 3. 返回语义（两种失败要分清）

| 情形 | 返回 | `EconomyResult` |
|---|---|---|
| 命令格式错误（缺幂等键、非法币种…） | `Fail(Error)` | — |
| 业务规则拒绝（余额不足、背包满、价格缺失…） | `Ok` | `applied=false`，`code` = 具体错误码 |
| 成功 | `Ok` | `applied=true`，`balance_after`/`version`/`created_guids` 有效 |

业务拒绝以 `Ok` 返回，是为了让调用方在同一通道读到**结构化错误码**与**快照余额**，
而不是只能拿到一条消息。

## 4. 原子性（§21）

`Purchase`（扣币 + 发物品）、`Transfer`（出账 + 入账）、`Reward`/`Refund`（货币 + 物品）
全部是**前向执行 + 失败逐项回滚**：任一步失败即撤销已完成的部分，再返回拒绝。

- `RemoveByDef` **先校验总量再扣**，避免扣到一半才发现不够（产生无法回滚的部分扣除）。
- 若连回滚都失败（理论上不可能，因回滚是撤销刚成功的对称操作），返回 `INTERNAL_ERROR`
  并如实暴露，**禁止假装回滚成功**。

## 5. 幂等（§15.10）

只缓存**已生效**的结果。被业务规则拒绝的操作**不写**幂等表：

> 否则「余额不足 → 充值后重试同一 key」会被永久毒化，那就不是幂等，是拒绝服务。

命中幂等表时返回首次结果并置 `deduplicated=true`，不重复生效。

## 6. 账本与失败语义（§19）

`ILedgerSink::Enqueue` 必须**非阻塞**（只入队，由 Persistence 线程落库，§9 禁止 Tick 内等待）。

投递失败时本实现**写死的一种**：内存态已生效 → 返回成功但置 `ledger_pending=true`，
**不回滚**。理由：经济操作已在 Scene 内生效，回滚会引入跨模块补偿复杂度；
最终一致性由 TASK-030 的账本重投 + 对账保证。

`LedgerEntry` 的 `reason`/`source` 按值持有 `std::string`——命令里是 `string_view`，
只在调用期有效，异步投递必须拷贝。

## 7. 价格表（§20.6）

价格**只能**来自 `config/gameplay/economy/prices.json`，代码内禁止任何硬编码价格：

```json
{ "version": 1,
  "prices": [ { "item_id": 1, "currency": 1, "unit_price": 50,
                "min_count": 1, "max_count": 99 } ] }
```

- `currency`：1=Gold 2=Silver 3=Gem 4=Honor
- `max_count = 0` 表示不限；`min_count` 缺省为 1
- 加载是**原子**的：任一条目非法则整表丢弃，禁止半应用（§21）
- 未配置的商品 → `NOT_FOUND` 拒绝，绝不回退到任何默认值

## 8. 事件（§15.9）

经 `SceneContext::events`（EventBus）发布，只入队、由宿主线程在 Event 阶段 Drain：

| 事件 | 触发 |
|---|---|
| `CurrencyChanged` | 任何余额变动（Transfer 双方各一条） |
| `ItemTraded` | Transfer 的物品部分 |
| `PurchaseCompleted` | 购买扣币与发物品**都成功**后才发布 |

事件体是 POD，不携带 `string_view`/裸指针——审计文本走账本而非事件体，避免跨 Tick 悬垂。

## 9. 性能（§22，Release 实测见 commit 正文）

| 指标 | 预算 | 实现要点 |
|---|---|---|
| `execute_ns` | ≤ 2000 | 八操作混合平均 |
| `balance_query_ns` | ≤ 50 | 钱包表是开放寻址扁平哈希（splitmix64 + 线性探测），无 `unordered_map` 指针追逐 |
| `dedup_check_ns` | ≤ 200 | 本地 `unordered_map<string, EconomyResult>` |

> 注意：所有指标都**批量摊销计时**（外层取一次时钟）。若逐次计时，QPC 的 ~17ns 硬地板
> 会把 `balance_query_ns` 的 50ns 预算吃掉三成以上，测出来的是时钟而不是代码。

## 10. 已知边界

- 第一版**不做**：拍卖行撮合、跨服交易、邮件附件（§8 明确留给后续）。
- Transfer 仅同 Scene；跨 Scene 走 DataService 事务（后续任务）。
- 持久化只能经 DataService，GameNode 禁止直连 MySQL/Redis（§13）。

---

# TASK-030 · Economic Ledger / Idempotency 接口说明

> 本节是 **TASK-030 的增量**，不推翻上面 TASK-029 的任何契约。
> §5「幂等」与 §6「账本」两节的**进程内兼容路径保留且仍然生效**（两者都不装配时行为不变），
> 生产装配必须同时 `SetIdempotencyStore()` + `SetLedger()`，否则「重复请求不扣两次钱」只在
> 单进程内成立。

## 11. 三个新公开头

| 头文件 | 导出 | 角色 |
|---|---|---|
| `ledger/ledger_entry.h` | `LedgerEntry`（十六字段）、`Hash256`、`ComputeEntryHash`、`VerifyEntryHash`、`ValidateLedgerEntry`、`VerifyChainOf` | 账本条目与哈希链（纯值类型 + 纯函数） |
| `ledger/idempotency_store.h` | `IdemStatus`、`IdemRow`、`IIdemTable`/`InMemoryIdemTable`、`IIdempotencyStore`/`IdempotencyStore` | 幂等四状态机 + TTL（持久层抽象与状态机分离） |
| `ledger/ledger_store.h` | `ILedgerStore`/`InMemoryLedgerStore`、`ILedgerStore::AppendResult` | 账本持久层抽象（DataService 侧实现） |

`ledger/ledger.h` 再导出 `Ledger`（Append / Flush / VerifyChain / QueryByPlayer / QueryByKey /
Stats / MemoryBytesPerEntry）与 `LedgerConfig` / `LedgerStats`。

**`sha256.h` 是本模块内部头（`src/ledger/`），不在公开头里**：公开头一律不 include 它，
只有本模块自己的白盒测试通过 `target_include_directories` 拿到它跑 FIPS 已知向量。

## 12. 幂等状态机（唯一真相）

```
Fresh ──TryBegin──► InFlight ──Commit──► Completed ──重复请求──► 返回首次结果(deduplicated=true)
                       │
                       ├──Abort──► 行删除（回到 Fresh，允许同 key 重试）
                       └──TTL 过期──► Failed ──► 查账本决定「重放」还是「重做」
```

| 状态 | `TryBegin` 返回 | `Execute` 的行为 |
|---|---|---|
| Fresh（不存在） | `Fresh` | 正常执行 |
| InFlight（未过期） | `InFlight` | **直接 BUSY，不执行**（并发/重试拦截） |
| Completed | `Completed` | 返回首次结果，`deduplicated=true`（**不是报错**） |
| Failed（TTL 过期） | `Failed` | `TryRecoverFromLedger`：账本命中 → 重放首次结果；未命中 → 释放后按首次执行 |

三条容易被实现错的判定（改动前先读 `src/ledger/idempotency_store.cpp` 文件头）：

1. 命中 InFlight 时**不得刷新 deadline**——否则卡死的执行者可被客户端重试无限续命，TTL 形同虚设。
2. TTL 过期转 Failed **不删除行**——真实结果可能已落账，删掉就等于放弃「不重复扣钱」的判据。
3. `Commit` 必须整行覆盖（含 `result`）——否则 Completed 后的重复请求只能拿到空结果，
   客户端会把 `applied=false` 误判为失败。

**TTL 是强制的**（§21）：`ttl <= 0` 被 `TryBegin` 拒绝（`INVALID_ARGUMENT`），默认 30s
（`kDefaultIdempotencyTtlMs`）。`SetIdempotencyTtlMs()` 可覆盖。

## 13. 账本（append-only + 哈希链）

**没有 update / delete 接口**，这是刻意的（§21）。

- `hash_i = SHA256(prev_hash_i ‖ CanonicalFieldOrder() 的逐字段流式编码)`
- 规范化顺序是**唯一真相**（`CanonicalFieldOrder()`，16 项，`static_assert` 保证数量一致）；
  **禁止把整条记录序列化后哈希**——字段顺序/对齐一变就整链断裂
- 链在 `Flush` 时按追加顺序计算（省 64B/条的槽内摘要）；`Flush` 严格 FIFO，所以与
  「Append 时算」结果完全一致。唯一例外是大条目直写路径（`item_deltas > 4`），
  它先 `Flush` 排空以保证顺序不被打乱
- `Flush` 失败**保留全部待落库条目**（含已写成功的；重投会命中 UNIQUE 并推进），不静默丢弃
- 落库 UNIQUE 冲突**不是错误**：`ILedgerStore::Append` 返回 `{stored=false, duplicate=true}`，
  计数进 `Stats().duplicates_rejected`

## 14. 背压契约（`Ledger::Append` 的错误码）

| 情况 | 错误码 |
|---|---|
| 缺 transaction_id / idempotency_key / player | `INVALID_ARGUMENT` |
| 待落库批次内已有同 idempotency_key | `VERSION_CONFLICT`（`duplicates_rejected++`） |
| 队列满且 Flush 排空失败 | **`BUSY`** |
| 大条目直写需要预排空，而排空失败 | **`BUSY`** |

**背压一律 BUSY，不复用底层持久层的错误码**：调用方在 `Append` 失败时要做的事只有一件事
——稍后重试/降级；把 MySQL 的 `TIMEOUT`、Redis 的连接错误透出来，会让调用方误以为
「换个错误码重试就能成功」。持久层的真实故障原因仍完整可观测：
`Stats().flush_failures`（最终失败次数）与 `Stats().pending`（被保留的条目数）。

## 15. §7 之外的必需扩展（显式记录，沿用 TASK-029 对 `peer` 的处理方式）

| 扩展 | 为什么必需 |
|---|---|
| `LedgerEntry::peer` | Transfer 的账本必须能还原双人资金流向，否则对账工具无法把「出账方 delta」与「入账方 delta」配对。它使结构正好是 §15.1 要求的**十六项字段** |
| `Ledger::QueryByKey` | InFlight 的 TTL 过期后，调用方必须能判断「该操作到底落账了没有」，否则无法区分「重放」与「重做」（§15.8） |
| `IdempotencyStore::SetInjectedNowMs` | 证明 TTL 行为**不依赖墙钟**（TTL 测试必须可复现） |
| `IdempotencyStore::Stats/ReapedCount/InFlightCount/BusyCount` | 「无悬挂 InFlight」「TTL 生效」是验收项，必须可观测 |

## 16. 对账口径（必须与 C++ 侧同口径）

```
net(p, c) = Σ{player == p, currency == c} delta  −  Σ{peer == p, currency == c} delta
```

为什么**减去** peer 项：Transfer 每笔命令只写**一行**账本，出账方 delta 为 `−amount`，
入账方那一侧没有独立行。只累加 `player == p` 的话，收款方的余额变化永远无法被账本解释，
对账必然「有差异」。实现见 `InMemoryLedgerStore::NetDeltaByPlayer` 与
`tools/audit/economy_audit.py`（两处口径必须一致）。

## 17. 恢复路径的已知限制（必须对客户端讲清楚）

`TryRecoverFromLedger` 从账本重建 `EconomyResult` 时：

- `balance_after` / `version` / `applied` / `deduplicated` 可完整还原；
- **`created_guids` 无法还原**——账本按 §7 不存新生成的 ItemGuid（那是背包的职责）。
  恢复路径上客户端应以**背包查询**为准，不要依赖账本回放发装备 guid；
- 余额本身不跨进程：`WalletTable` 的 Owner 是承载该玩家的 GameNode（§6），
  崩溃后由 DataService 的持久化余额装载（TASK-026/027/028 的 Load），
  账本用于「对账与重放判定」，不是余额的唯一恢复来源（§21 同样禁止把 Redis RDB
  当唯一恢复方案）。

## 18. 性能（§22，Release 实测，`bench/ledger.txt`）

| 指标 | 预算 | 实测（9 轮中位数） |
|---|---|---|
| `idem_check_ns` | ≤ 200 | **159.64**（min 152.82 / max 210.18） |
| `ledger_append_ns` | ≤ 500 | **45.72** |
| `dedup_hit_ns` | — | 147.43 |
| `flush_ns_per_1k` | ≤ 50 ms | **0.787 ms** |
| `mem_bytes_per_entry` | ≤ 256 | **150** |

> 口径说明（§3.3「性能假数据」）：必须与丢弃/重复计数一起看。本次
> `appended=10000 / fresh=10000 / dedup_hits=10000/10000`，即**没有任何条目被丢弃或
> 重复计数**，数字不是走快路径空转出来的。
>
> 计时估计量是 **9 轮中位数，并把 min/max 一并落盘**。不用单批均值的原因：
> 本机同一份代码同一工作量的单批均值离散到 143.7~230.3 ns（极差 1.6 倍），
> 单批均值偶发就越过 200 ns，会形成 flaky 门禁。中位数不改工作量，只把「偶然被打断的
> 那一轮」排除在中心趋势外；离散度同时可见，避免用中位数掩盖离群。
>
> ⚠ **`idem_check_ns` 对表规模高度敏感**（实测 `--ops` 10k→500k 时
> 143.87→404.99 ns）。原因是 `unordered_map<string, IdemRow>` 每节点 ≈112B，
> 20 万条约 22MB 远超 L3，每次查找/插入付 1~2 次 cache miss。
> 即 §22 的 200ns 只在**工作集驻留缓存**时成立：幂等表的历史键必须过期清理
> （Redis 靠 TTL、MySQL 靠分区/归档），否则延迟会随运行时长劣化。
> 单节点键量若上到 10⁵ 量级，应换开放寻址扁平表、并把 `EconomyResult` 移出热点行
> （状态机只需要 `status + deadline`）。
> 内存模型见 `ledger.h` 文件头：定长紧凑槽 + 变长 arena，避免 `LedgerEntry`
> （3 个 `std::string` + 1 个 `vector`）每条 4 次堆分配、轻松超 300B 的问题。

## 19. 数据层落地

- `database/migrations/003_ledger.sql`：`economy_ledger`（append-only，`transaction_id` 主键 +
  `idempotency_key` **UNIQUE** + 玩家/时间窗索引）+ `economy_idempotency`（`idempotency_key` 主键
  → UNIQUE 兜底，`status`/`deadline_ms`/`result_json`）。
- **命名必须匹配 `NNN_name.sql`（版本 ≥ 1）**：写成 `00N_ledger.sql` 会被
  `MigrationRunner::ParseVersion` **静默跳过**，构建全绿但表根本不存在。
- ⚠ **规格冲突已记录在 SQL 文件头**：§8 同时要求 `idempotency_key` UNIQUE 与
  `timestamp_ms` 按月分区，而 MariaDB/MySQL 规定「每个唯一索引必须包含分区表达式的全部列」
  → 两者不可能在同一张表上同时成立。按 §30 优先级（Correctness > Scalability）
  **保留全局 UNIQUE、不做按月分区**，并把将来分区的既定迁移路径写在文件头。

