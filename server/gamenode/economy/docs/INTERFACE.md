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
