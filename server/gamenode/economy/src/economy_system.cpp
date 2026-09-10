// server/gamenode/economy/src/economy_system.cpp — TASK-029 §7 / §8 / §15 / §19
//
// 执行顺序（§8）：校验参数 → 查幂等表 → 检查余额/背包 → 扣/加 → 异步投递账本 → 发布事件 → 返回。
//
// 原子性约定（§21）：Purchase「扣币 + 发物品」、Transfer「出账 + 入账」、Reward/Refund
// 「货币 + 物品」必须同成功同失败。实现方式是**前向执行 + 失败逐项回滚**：
//   单线程 Scene 内没有其他写入者，回滚只会遇到「刚加进去就被溢出拒绝」这种对称失败，
//   若连回滚都失败（理论上不可能，因回滚是撤销刚才成功的加法），记 INTERNAL_ERROR 并
//   保持已回滚部分不变，禁止「假装回滚成功」。
//
// 幂等（§15.10）：只缓存**已生效**的结果。被业务规则拒绝的操作不写幂等表——
//   否则「余额不足 → 充值后重试同一 key」会被永久毒化，这不是幂等而是拒绝服务。

#include "mmo/game/economy/economy_system.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "mmo/game/inventory/inventory.h"

namespace mmo::game::economy {

namespace {

core::Error EconErr(core::ErrorCode code, const char* msg) noexcept {
    return core::Error(code, msg, core::domain::kEconomy);
}

/// 业务拒绝：命令合法但不满足执行条件（余额不足/背包满/价格缺失）。
/// 以 Ok 携带 applied=false + code 返回，与「命令格式错误」的 Fail 区分开（§19）。
core::Result<EconomyResult> Reject(core::ErrorCode code, std::int64_t balance_after,
                                   std::uint32_t version) noexcept {
    EconomyResult out;
    out.applied = false;
    out.code = code;
    out.balance_after = balance_after;
    out.version = version;
    return core::Result<EconomyResult>::Ok(std::move(out));
}

/// 当前玩家背包内所有物品实例 guid（用于 diff 出本次新增的 guid）。
std::vector<inventory::ItemGuid> SnapshotGuids(inventory::InventorySystem& inv, PlayerId p) {
    std::vector<inventory::ItemGuid> out;
    const inventory::Inventory* v = inv.View(p);
    if (v == nullptr) return out;
    for (std::size_t i = 0; i < inventory::kMaxInventorySlots; ++i) {
        const inventory::ItemStack* s = v->At(static_cast<inventory::SlotIndex>(i));
        if (s != nullptr && s->guid != inventory::kInvalidItemGuid) out.push_back(s->guid);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// after 中新增于 before 的 guid（背包槽位上限 100，线性 diff 足够且无分配抖动）。
std::vector<inventory::ItemGuid> DiffNew(const std::vector<inventory::ItemGuid>& before,
                                         const std::vector<inventory::ItemGuid>& after) {
    std::vector<inventory::ItemGuid> out;
    std::size_t i = 0;
    for (const auto g : after) {
        while (i < before.size() && before[i] < g) ++i;
        if (i >= before.size() || before[i] != g) out.push_back(g);
    }
    return out;
}

}  // namespace

EconomySystem::EconomySystem(inventory::InventorySystem& inv) noexcept : inv_(inv) {}

// ---------------------------------------------------------------- 主入口

core::Result<EconomyResult> EconomySystem::Execute(const EconomyCommand& cmd,
                                                   const SceneContext& ctx) {
    ++stats_.op_count;

    // 1) 参数校验（§8 必填字段）→ 格式错误一律 Fail，不进幂等表
    auto v = ValidateCommand(cmd);
    if (!v.HasValue()) {
        ++stats_.fail_count;
        return core::Result<EconomyResult>::Fail(v.Err());
    }

    // 2) 幂等（TASK-030 §15.6）：TryBegin 在**任何副作用之前**，这是「不重复扣钱」的全部依据。
    if (idem_ != nullptr) {
        auto began = idem_->TryBegin(cmd.idempotency_key, idem_ttl_);
        if (!began.HasValue()) {
            ++stats_.fail_count;
            return core::Result<EconomyResult>::Fail(began.Err());
        }
        switch (began.Value()) {
            case ledger::IdemStatus::Completed: {
                // 已完成：返回**首次结果**而不是报错（§20.6 对客户端友好）。
                auto prev = idem_->Lookup(cmd.idempotency_key);
                if (prev.HasValue() && prev.Value().has_value()) {
                    EconomyResult res = *prev.Value();
                    res.deduplicated = true;
                    ++stats_.dedup_count;
                    return core::Result<EconomyResult>::Ok(std::move(res));
                }
                // Completed 却查不到结果：表被外部改写。按 BUSY 拒绝，绝不重新执行。
                ++busy_count_;
                ++stats_.fail_count;
                return RejectFromWallets(cmd, core::ErrorCode::BUSY);
            }
            case ledger::IdemStatus::InFlight: {
                // 并发执行者持有该 key：直接 BUSY，**不重复执行**（§8）。
                ++busy_count_;
                ++stats_.fail_count;
                return RejectFromWallets(cmd, core::ErrorCode::BUSY);
            }
            case ledger::IdemStatus::Failed: {
                // TTL 过期的未决态（§15.8）：先查账本决定「重放」还是「重做」。
                std::optional<EconomyResult> recovered = TryRecoverFromLedger(cmd);
                if (recovered.has_value()) {
                    ++stats_.dedup_count;
                    return core::Result<EconomyResult>::Ok(std::move(*recovered));
                }
                // 账本里没有该 key = 该操作从未落账 → 释放后按首次执行。
                if (!RebeginIdempotency(cmd)) {
                    ++busy_count_;
                    ++stats_.fail_count;
                    return RejectFromWallets(cmd, core::ErrorCode::BUSY);
                }
                break;
            }
            case ledger::IdemStatus::Fresh:
            default:
                break;
        }
    }

    // 3) 兼容路径（未装配 TASK-030 的幂等存储时）：进程内 map 兜底，语义与 TASK-029 一致。
    if (idem_ == nullptr) {
        auto it = idempotency_.find(cmd.idempotency_key);
        if (it != idempotency_.end()) {
            ++stats_.dedup_count;
            EconomyResult res = it->second;
            res.deduplicated = true;
            return core::Result<EconomyResult>::Ok(std::move(res));
        }
    }

    // 4) 记录执行前余额：账本的 delta 由它算（不能用 cmd.amount，见 PostLedger 注释）。
    std::int64_t balance_before = 0;
    {
        auto b = Balance(cmd.player, cmd.currency);
        if (b.HasValue()) balance_before = b.Value();
    }

    // 5) 分派（Result 无默认构造，初值即「未知操作」；正常路径必被覆盖）
    core::Result<EconomyResult> r = core::Result<EconomyResult>::Fail(
        EconErr(core::ErrorCode::INVALID_ARGUMENT, "economy: unhandled op"));
    switch (cmd.op) {
        case EconomyOp::AddCurrency:
        case EconomyOp::RemoveCurrency:
            r = DoCurrency(cmd, ctx);
            break;
        case EconomyOp::AddItem:
        case EconomyOp::RemoveItem:
            r = DoItems(cmd, ctx);
            break;
        case EconomyOp::Transfer:
            r = DoTransfer(cmd, ctx);
            break;
        case EconomyOp::Purchase:
            r = DoPurchase(cmd, ctx);
            break;
        case EconomyOp::Reward:
            r = DoReward(cmd, ctx);
            break;
        case EconomyOp::Refund:
            r = DoRefund(cmd, ctx);
            break;
        default:
            r = core::Result<EconomyResult>::Fail(
                EconErr(core::ErrorCode::INVALID_ARGUMENT, "economy: unknown op"));
            break;
    }
    if (!r.HasValue()) {
        ++stats_.fail_count;
        // 内部失败（如回滚失败）→ 释放幂等键，允许重试；绝不留下 InFlight 悬挂。
        if (idem_ != nullptr) (void)idem_->Abort(cmd.idempotency_key);
        return r;
    }

    EconomyResult res = std::move(r).Value();
    if (!res.applied) {
        ++stats_.fail_count;
        // 业务拒绝（余额不足/背包满）不写幂等表：否则「充值后用同一 key 重试」会被永久毒化。
        if (idem_ != nullptr) (void)idem_->Abort(cmd.idempotency_key);
        return core::Result<EconomyResult>::Ok(std::move(res));
    }

    ++stats_.applied_count;
    // 6) 异步投递账本（§9 禁止同步等待）；投递失败 → 标记 pending（§19 写死的一种）
    PostLedger(cmd, res, res.balance_after - balance_before);
    // 7) 落幂等表（必须在返回前完成，§9）
    if (idem_ != nullptr) {
        auto committed = idem_->Commit(cmd.idempotency_key, res);
        if (!committed.HasValue()) {
            // 内存态已变但幂等未落：**不能**当作成功返回（客户端重试会再执行一次）。
            // 拒绝并把事实说清楚；此时该 key 仍是 InFlight，重试会被 BUSY 拦住；
            // TTL 过期后走 TryRecoverFromLedger，从账本恢复出首次结果（自愈闭环）。
            ++stats_.fail_count;
            return core::Result<EconomyResult>::Fail(EconErr(
                core::ErrorCode::INTERNAL_ERROR,
                "economy: idempotency commit failed; ledger replay will reconcile"));
        }
    } else {
        idempotency_.emplace(cmd.idempotency_key, res);
    }
    return core::Result<EconomyResult>::Ok(std::move(res));
}

// ---------------------------------------------------------------- 幂等恢复（§15.8）

core::Result<EconomyResult> EconomySystem::RejectFromWallets(const EconomyCommand& cmd,
                                                             core::ErrorCode code) {
    std::int64_t bal = 0;
    auto b = Balance(cmd.player, cmd.currency);
    if (b.HasValue()) bal = b.Value();
    const Wallet* w = wallets_.Find(cmd.player);
    return Reject(code, bal, w != nullptr ? w->version : 0u);
}

std::optional<EconomyResult> EconomySystem::TryRecoverFromLedger(const EconomyCommand& cmd) {
    if (ledger_store_ == nullptr || idem_ == nullptr) return std::nullopt;

    auto hit = ledger_store_->QueryByKey(cmd.idempotency_key);
    if (!hit.HasValue() || !hit.Value().has_value()) return std::nullopt;

    const ledger::LedgerEntry& e = *hit.Value();
    EconomyResult res;
    res.applied = true;
    res.deduplicated = true;
    res.code = core::ErrorCode::OK;
    res.balance_after = e.balance_after;
    res.version = e.version;
    // created_guids 无法从账本还原（账本按 §7 不存新生成 guid）；恢复路径的客户端应
    // 以背包查询为准。这一点在 docs/INTERFACE.md 与故障报告里显式记录。
    res.ledger_pending = false;

    // 把幂等表补记为 Completed，后续同 key 请求直接命中首次结果。
    (void)idem_->Commit(cmd.idempotency_key, res);
    return std::optional<EconomyResult>(std::move(res));
}

bool EconomySystem::RebeginIdempotency(const EconomyCommand& cmd) noexcept {
    (void)idem_->Abort(cmd.idempotency_key);  // 回到 Fresh（行删除）
    auto again = idem_->TryBegin(cmd.idempotency_key, idem_ttl_);
    return again.HasValue() && again.Value() == ledger::IdemStatus::Fresh;
}

core::Result<std::int64_t> EconomySystem::Balance(PlayerId player,
                                                  CurrencyType currency) const noexcept {
    if (currency == 0 || currency > kCurrencyTypeMax) {
        return core::Result<std::int64_t>::Fail(
            EconErr(core::ErrorCode::NOT_FOUND, "economy: unknown currency"));
    }
    const Wallet* w = wallets_.Find(player);
    if (w == nullptr) return core::Result<std::int64_t>::Ok(0);  // 无钱包 = 零余额
    return w->Get(currency);
}

core::Result<void> EconomySystem::SetPriceTable(const PriceTable& table) {
    prices_ = table;
    return core::Result<void>::Ok();
}

std::int64_t EconomySystem::TotalBalance() const noexcept { return wallets_.SumAllBalances(); }

// ---------------------------------------------------------------- 货币

core::Result<EconomyResult> EconomySystem::DoCurrency(const EconomyCommand& cmd,
                                                      const SceneContext& ctx) {
    Wallet& w = wallets_.Fetch(cmd.player);
    const bool add = (cmd.op == EconomyOp::AddCurrency);
    const std::int64_t delta = add ? cmd.amount : -cmd.amount;

    auto before = w.Get(cmd.currency);
    const std::int64_t before_v = before.HasValue() ? before.Value() : 0;

    auto r = w.Add(cmd.currency, delta);
    if (!r.HasValue()) {
        return Reject(r.Err().Code(), before_v, w.version);
    }

    if (add) {
        stats_.total_minted += cmd.amount;
    } else {
        stats_.total_burned += cmd.amount;
    }

    auto after = w.Get(cmd.currency);
    const std::int64_t after_v = after.HasValue() ? after.Value() : 0;

    EconomyResult out;
    out.applied = true;
    out.code = core::ErrorCode::OK;
    out.balance_after = after_v;
    out.version = w.version;
    EmitCurrency(ctx, cmd.player, cmd.currency, delta, after_v, cmd.trace_id);
    return core::Result<EconomyResult>::Ok(std::move(out));
}

// ---------------------------------------------------------------- 物品

core::Result<EconomyResult> EconomySystem::DoItems(const EconomyCommand& cmd,
                                                   const SceneContext& ctx) {
    (void)ctx;  // 纯物品增减不产生 §15.9 定义的三类事件（事件由货币/交易/购买路径发布）
    Wallet& w = wallets_.Fetch(cmd.player);
    auto bal = w.Get(cmd.currency);
    const std::int64_t bal_v = bal.HasValue() ? bal.Value() : 0;

    EconomyResult out;
    out.balance_after = bal_v;  // 物品操作不改货币，balance_after 仍给出该币种余额

    // 反向操作按倒序回滚，保证「先加的后撤」，避免背包满时回滚自己也失败。
    const bool adding = (cmd.op == EconomyOp::AddItem);

    if (adding) {
        const std::vector<inventory::ItemGuid> before = SnapshotGuids(inv_, cmd.player);
        std::size_t done = 0;
        for (const ItemDelta& d : cmd.item_deltas) {
            auto r = inv_.Add(cmd.player, d.def_id, static_cast<std::uint32_t>(d.count),
                              cmd.trace_id);
            if (!r.HasValue()) {
                // 回滚已加入的部分（只回滚数量，不区分 guid：Add 的逆是 Remove）
                for (std::size_t i = 0; i < done; ++i) {
                    const ItemDelta& p = cmd.item_deltas[i];
                    (void)RemoveByDef(cmd.player, p.def_id, static_cast<std::uint32_t>(p.count),
                                      cmd.trace_id);
                }
                return Reject(r.Err().Code(), bal_v, w.version);
            }
            ++done;
        }
        const std::vector<inventory::ItemGuid> after = SnapshotGuids(inv_, cmd.player);
        out.created_guids = DiffNew(before, after);
    } else {
        std::size_t done = 0;
        for (const ItemDelta& d : cmd.item_deltas) {
            const std::uint32_t n = static_cast<std::uint32_t>(-d.count);
            auto r = (d.guid != inventory::kInvalidItemGuid)
                         ? inv_.Remove(cmd.player, d.guid, n, cmd.trace_id)
                         : RemoveByDef(cmd.player, d.def_id, n, cmd.trace_id);
            if (!r.HasValue()) {
                // 回滚：把已移除的物品加回去（guid 不保证复原，但数量与 def_id 守恒）
                for (std::size_t i = 0; i < done; ++i) {
                    const ItemDelta& p = cmd.item_deltas[i];
                    (void)inv_.Add(cmd.player, p.def_id, static_cast<std::uint32_t>(-p.count),
                                   cmd.trace_id);
                }
                return Reject(r.Err().Code(), bal_v, w.version);
            }
            ++done;
        }
    }

    out.applied = true;
    out.code = core::ErrorCode::OK;
    out.version = w.version;
    return core::Result<EconomyResult>::Ok(std::move(out));
}

// ---------------------------------------------------------------- 转移（§15.6）

core::Result<EconomyResult> EconomySystem::DoTransfer(const EconomyCommand& cmd,
                                                      const SceneContext& ctx) {
    Wallet& from = wallets_.Fetch(cmd.player);
    Wallet& to = wallets_.Fetch(cmd.peer);

    auto bal = from.Get(cmd.currency);
    const std::int64_t bal_v = bal.HasValue() ? bal.Value() : 0;

    EconomyResult out;
    out.balance_after = bal_v;

    // ---- 货币部分：先出账，后入账；入账失败（溢出）则回滚出账 ----
    if (cmd.amount > 0) {
        auto d = from.Add(cmd.currency, -cmd.amount);
        if (!d.HasValue()) return Reject(d.Err().Code(), bal_v, from.version);

        auto a = to.Add(cmd.currency, cmd.amount);
        if (!a.HasValue()) {
            auto rb = from.Add(cmd.currency, cmd.amount);  // 撤销出账
            if (!rb.HasValue()) {
                return core::Result<EconomyResult>::Fail(EconErr(
                    core::ErrorCode::INTERNAL_ERROR, "economy: transfer rollback failed"));
            }
            return Reject(a.Err().Code(), bal_v, from.version);
        }
        // 转移不 mint 也不 burn：总量守恒（§17）
        auto after = from.Get(cmd.currency);
        out.balance_after = after.HasValue() ? after.Value() : 0;
        out.version = from.version;
        EmitCurrency(ctx, cmd.player, cmd.currency, -cmd.amount, out.balance_after, cmd.trace_id);
        EmitCurrency(ctx, cmd.peer, cmd.currency, cmd.amount, cmd.amount, cmd.trace_id);
    }

    // ---- 物品部分：先出方移除，后入方加入；加入失败则把物品还给出方 ----
    for (const ItemDelta& d : cmd.item_deltas) {
        const std::uint32_t n = static_cast<std::uint32_t>(d.count > 0 ? d.count : -d.count);
        auto rm = (d.guid != inventory::kInvalidItemGuid)
                      ? inv_.Remove(cmd.player, d.guid, n, cmd.trace_id)
                      : RemoveByDef(cmd.player, d.def_id, n, cmd.trace_id);
        if (!rm.HasValue()) return Reject(rm.Err().Code(), out.balance_after, from.version);

        auto add = inv_.Add(cmd.peer, d.def_id, n, cmd.trace_id);
        if (!add.HasValue()) {
            auto rb = inv_.Add(cmd.player, d.def_id, n, cmd.trace_id);  // 原样还回
            if (!rb.HasValue()) {
                return core::Result<EconomyResult>::Fail(EconErr(
                    core::ErrorCode::INTERNAL_ERROR, "economy: transfer item rollback failed"));
            }
            return Reject(add.Err().Code(), out.balance_after, from.version);
        }
        ItemTraded ev;
        ev.from = cmd.player;
        ev.to = cmd.peer;
        ev.def_id = d.def_id;
        ev.count = n;
        ev.trace = cmd.trace_id;
        (void)ctx.events.Publish(ev);
    }

    out.applied = true;
    out.code = core::ErrorCode::OK;
    return core::Result<EconomyResult>::Ok(std::move(out));
}

// ---------------------------------------------------------------- 购买（§15.7）

core::Result<EconomyResult> EconomySystem::DoPurchase(const EconomyCommand& cmd,
                                                      const SceneContext& ctx) {
    const ItemDelta& d = cmd.item_deltas.front();

    // 价格必须来自价格表（§20.6 禁止硬编码）
    const PriceEntry* pe = prices_.Find(d.def_id);
    if (pe == nullptr) {
        // 价格表未配置该商品 → 拒绝（§20.6：绝不回退到任何硬编码价格）
        auto bal = wallets_.Fetch(cmd.player).Get(cmd.currency);
        return Reject(core::ErrorCode::NOT_FOUND, bal.HasValue() ? bal.Value() : 0,
                      wallets_.Fetch(cmd.player).version);
    }
    if (!pe->Allows(static_cast<std::uint32_t>(d.count))) {
        auto bal = wallets_.Fetch(cmd.player).Get(pe->currency);
        return Reject(core::ErrorCode::INVALID_ARGUMENT, bal.HasValue() ? bal.Value() : 0,
                      wallets_.Fetch(cmd.player).version);
    }
    std::int64_t total = 0;
    if (!pe->Total(static_cast<std::uint32_t>(d.count), total)) {
        auto bal = wallets_.Fetch(cmd.player).Get(pe->currency);
        return Reject(core::ErrorCode::BUSY, bal.HasValue() ? bal.Value() : 0,
                      wallets_.Fetch(cmd.player).version);  // 总价溢出
    }

    Wallet& w = wallets_.Fetch(cmd.player);
    auto bal0 = w.Get(pe->currency);
    const std::int64_t before_v = bal0.HasValue() ? bal0.Value() : 0;

    // 1) 扣币
    auto pay = w.Add(pe->currency, -total);
    if (!pay.HasValue()) return Reject(pay.Err().Code(), before_v, w.version);

    // 2) 发物品；失败必须把钱退回去（§21：禁止扣钱成功但发物品失败）
    const std::vector<inventory::ItemGuid> before = SnapshotGuids(inv_, cmd.player);
    auto add = inv_.Add(cmd.player, d.def_id, static_cast<std::uint32_t>(d.count), cmd.trace_id);
    if (!add.HasValue()) {
        auto rb = w.Add(pe->currency, total);
        if (!rb.HasValue()) {
            return core::Result<EconomyResult>::Fail(
                EconErr(core::ErrorCode::INTERNAL_ERROR, "economy: purchase rollback failed"));
        }
        return Reject(add.Err().Code(), before_v, w.version);  // 已回滚，余额回到 before_v
    }

    stats_.total_burned += total;

    auto bal1 = w.Get(pe->currency);
    const std::int64_t after_v = bal1.HasValue() ? bal1.Value() : 0;

    EconomyResult out;
    out.applied = true;
    out.code = core::ErrorCode::OK;
    out.created_guids = DiffNew(before, SnapshotGuids(inv_, cmd.player));
    out.balance_after = after_v;
    out.version = w.version;

    EmitCurrency(ctx, cmd.player, pe->currency, -total, after_v, cmd.trace_id);
    PurchaseCompleted ev;
    ev.buyer = cmd.player;
    ev.def_id = d.def_id;
    ev.count = static_cast<std::uint32_t>(d.count);
    ev.currency = pe->currency;
    ev.total_price = total;
    ev.trace = cmd.trace_id;
    (void)ctx.events.Publish(ev);
    return core::Result<EconomyResult>::Ok(std::move(out));
}

// ---------------------------------------------------------------- 奖励（§15.8）

core::Result<EconomyResult> EconomySystem::DoReward(const EconomyCommand& cmd,
                                                    const SceneContext& ctx) {
    Wallet& w = wallets_.Fetch(cmd.player);
    auto bal0 = w.Get(cmd.currency);
    const std::int64_t before_v = bal0.HasValue() ? bal0.Value() : 0;

    // 1) 货币
    if (cmd.amount > 0) {
        auto a = w.Add(cmd.currency, cmd.amount);
        if (!a.HasValue()) return Reject(a.Err().Code(), before_v, w.version);
        stats_.total_minted += cmd.amount;
    }

    // 2) 物品；失败则把刚发的货币撤回（同成功同失败）
    const bool has_items = !cmd.item_deltas.empty();
    const std::vector<inventory::ItemGuid> before =
        has_items ? SnapshotGuids(inv_, cmd.player) : std::vector<inventory::ItemGuid>{};
    std::size_t done = 0;
    for (const ItemDelta& d : cmd.item_deltas) {
        auto r = inv_.Add(cmd.player, d.def_id, static_cast<std::uint32_t>(d.count), cmd.trace_id);
        if (!r.HasValue()) {
            for (std::size_t i = 0; i < done; ++i) {
                const ItemDelta& p = cmd.item_deltas[i];
                (void)RemoveByDef(cmd.player, p.def_id, static_cast<std::uint32_t>(p.count),
                                  cmd.trace_id);
            }
            if (cmd.amount > 0) {
                auto rb = w.Add(cmd.currency, -cmd.amount);
                if (rb.HasValue()) stats_.total_minted -= cmd.amount;
            }
            return Reject(r.Err().Code(), before_v, w.version);
        }
        ++done;
    }

    auto bal1 = w.Get(cmd.currency);
    const std::int64_t after_v = bal1.HasValue() ? bal1.Value() : 0;

    EconomyResult out;
    out.applied = true;
    out.code = core::ErrorCode::OK;
    out.balance_after = after_v;
    out.version = w.version;
    if (has_items) out.created_guids = DiffNew(before, SnapshotGuids(inv_, cmd.player));
    if (cmd.amount > 0) {
        EmitCurrency(ctx, cmd.player, cmd.currency, cmd.amount, after_v, cmd.trace_id);
    }
    return core::Result<EconomyResult>::Ok(std::move(out));
}

// ---------------------------------------------------------------- 退款（Purchase 的逆）

core::Result<EconomyResult> EconomySystem::DoRefund(const EconomyCommand& cmd,
                                                    const SceneContext& ctx) {
    Wallet& w = wallets_.Fetch(cmd.player);
    auto bal0 = w.Get(cmd.currency);
    const std::int64_t before_v = bal0.HasValue() ? bal0.Value() : 0;

    // 1) 退还货币（mint）
    if (cmd.amount > 0) {
        auto a = w.Add(cmd.currency, cmd.amount);
        if (!a.HasValue()) return Reject(a.Err().Code(), before_v, w.version);
        stats_.total_minted += cmd.amount;
    }

    // 2) 物品：count > 0 = 退还给玩家（Add）；count < 0 = 从玩家收回（Remove）
    const bool has_items = !cmd.item_deltas.empty();
    const std::vector<inventory::ItemGuid> before =
        has_items ? SnapshotGuids(inv_, cmd.player) : std::vector<inventory::ItemGuid>{};
    std::size_t done = 0;
    for (const ItemDelta& d : cmd.item_deltas) {
        core::Result<std::uint32_t> r = (d.count > 0)
            ? inv_.Add(cmd.player, d.def_id, static_cast<std::uint32_t>(d.count), cmd.trace_id)
            : ((d.guid != inventory::kInvalidItemGuid)
                   ? inv_.Remove(cmd.player, d.guid, static_cast<std::uint32_t>(-d.count),
                                 cmd.trace_id)
                   : RemoveByDef(cmd.player, d.def_id, static_cast<std::uint32_t>(-d.count),
                                 cmd.trace_id));
        if (!r.HasValue()) {
            // 回滚已完成的物品变更（Add 的逆是 Remove，Remove 的逆是 Add）
            for (std::size_t i = 0; i < done; ++i) {
                const ItemDelta& p = cmd.item_deltas[i];
                if (p.count > 0) {
                    (void)RemoveByDef(cmd.player, p.def_id, static_cast<std::uint32_t>(p.count),
                                      cmd.trace_id);
                } else {
                    (void)inv_.Add(cmd.player, p.def_id, static_cast<std::uint32_t>(-p.count),
                                   cmd.trace_id);
                }
            }
            if (cmd.amount > 0) {
                auto rb = w.Add(cmd.currency, -cmd.amount);
                if (rb.HasValue()) stats_.total_minted -= cmd.amount;
            }
            return Reject(r.Err().Code(), before_v, w.version);
        }
        ++done;
    }

    auto bal1 = w.Get(cmd.currency);
    const std::int64_t after_v = bal1.HasValue() ? bal1.Value() : 0;

    EconomyResult out;
    out.applied = true;
    out.code = core::ErrorCode::OK;
    out.balance_after = after_v;
    out.version = w.version;
    if (has_items) out.created_guids = DiffNew(before, SnapshotGuids(inv_, cmd.player));
    if (cmd.amount > 0) {
        EmitCurrency(ctx, cmd.player, cmd.currency, cmd.amount, after_v, cmd.trace_id);
    }
    return core::Result<EconomyResult>::Ok(std::move(out));
}

// ---------------------------------------------------------------- 辅助

core::Result<std::uint32_t> EconomySystem::RemoveByDef(PlayerId p, inventory::ItemId def_id,
                                                       std::uint32_t count, core::TraceID trace) {
    if (count == 0) {
        return core::Result<std::uint32_t>::Fail(
            EconErr(core::ErrorCode::INVALID_ARGUMENT, "economy: remove count must be > 0"));
    }
    const inventory::Inventory* v = inv_.View(p);
    if (v == nullptr) {
        return core::Result<std::uint32_t>::Fail(
            EconErr(core::ErrorCode::NOT_FOUND, "economy: player has no inventory"));
    }

    // 第一遍：只统计可用量（禁止边统计边扣，避免部分扣除）
    std::uint32_t available = 0;
    for (std::size_t i = 0; i < inventory::kMaxInventorySlots && available < count; ++i) {
        const inventory::ItemStack* s = v->At(static_cast<inventory::SlotIndex>(i));
        if (s == nullptr || s->guid == inventory::kInvalidItemGuid) continue;
        if (s->def_id != def_id) continue;
        available += s->count;
    }
    if (available < count) {
        return core::Result<std::uint32_t>::Fail(
            EconErr(core::ErrorCode::BUSY, "economy: not enough items to remove"));
    }

    // 第二遍：按槽扣减（View 每次调用都返回同一对象，Remove 会改变内容，故逐次重查）
    std::uint32_t remaining = count;
    for (std::size_t i = 0; i < inventory::kMaxInventorySlots && remaining > 0; ++i) {
        const inventory::ItemStack* s = inv_.View(p)->At(static_cast<inventory::SlotIndex>(i));
        if (s == nullptr || s->guid == inventory::kInvalidItemGuid || s->def_id != def_id) {
            continue;
        }
        const std::uint32_t take = std::min(remaining, s->count);
        auto r = inv_.Remove(p, s->guid, take, trace);
        if (!r.HasValue()) return core::Result<std::uint32_t>::Fail(r.Err());
        remaining -= take;
    }
    if (remaining != 0) {
        // 理论不可达（已预校验）；真发生说明并发写入，明确报错而非静默
        return core::Result<std::uint32_t>::Fail(
            EconErr(core::ErrorCode::INTERNAL_ERROR, "economy: partial remove"));
    }
    return core::Result<std::uint32_t>::Ok(count);
}

// ---------------------------------------------------------------- 账本 / 事件

void EconomySystem::PostLedger(const EconomyCommand& cmd, EconomyResult& res,
                               std::int64_t delta) noexcept {
    // ---- TASK-030：完整账本（append-only + 哈希链 + 幂等键唯一）----
    // 投递失败与 sink 路径同语义：内存态已生效 → 标记 pending，不回滚（§19）。
    if (ledger_store_ != nullptr) {
        ledger::LedgerEntry e;
        e.transaction_id = cmd.transaction_id;
        e.request_id = cmd.request_id;
        e.idempotency_key = cmd.idempotency_key;
        e.player = cmd.player;
        e.peer = cmd.peer;
        e.op = cmd.op;
        e.currency = cmd.currency;
        e.delta = delta;
        e.balance_after = res.balance_after;
        e.item_deltas = cmd.item_deltas;  // 按值拷贝：账本在异步落库时不能再引用命令
        e.reason.assign(cmd.reason.data(), cmd.reason.size());
        e.source.assign(cmd.source.data(), cmd.source.size());
        e.timestamp_ms = cmd.timestamp_ms;
        e.version = res.version;

        auto appended = ledger_store_->Append(e);
        if (!appended.HasValue()) {
            res.ledger_pending = true;
            ++stats_.ledger_pending_count;
        }
    }

    if (ledger_ == nullptr) return;  // 未安装 sink = 不投递，pending 恒 false

    LedgerEntry e;
    e.transaction_id = cmd.transaction_id;
    e.player = cmd.player;
    e.peer = cmd.peer;
    e.op = cmd.op;
    e.currency = cmd.currency;
    e.amount = cmd.amount;
    e.balance_after = res.balance_after;
    e.version = res.version;
    e.idempotency_key = cmd.idempotency_key;
    // 异步投递：命令里的 string_view 只在调用期有效，此处必须按值拷贝。
    e.reason.assign(cmd.reason.data(), cmd.reason.size());
    e.source.assign(cmd.source.data(), cmd.source.size());
    e.timestamp_ms = cmd.timestamp_ms;

    if (!ledger_->Enqueue(e)) {
        res.ledger_pending = true;  // §19：内存态已改 → 成功但标记 pending，不回滚
        ++stats_.ledger_pending_count;
    }
}

void EconomySystem::EmitCurrency(const SceneContext& ctx, PlayerId pid, CurrencyType cur,
                                 std::int64_t delta, std::int64_t after,
                                 core::TraceID trace) noexcept {
    CurrencyChanged ev;
    ev.player = pid;
    ev.currency = cur;
    ev.delta = delta;
    ev.balance_after = after;
    ev.trace = trace;
    (void)ctx.events.Publish(ev);  // 发布失败不影响命令结果（事件是旁路）
}

}  // namespace mmo::game::economy
