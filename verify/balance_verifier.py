"""CAMI 经济系统与机制层 —— 平衡校验引擎 (Balance Verifier)

把每一份 *Balance / EconomyFlow 变成**可执行的验证路径**，输出：
  - 经济源汇对账（每币种 sources vs sinks vs 预期，闭合 Day3 GAP-4）
  - 币种周上限一致性（currencies / pvp_balance / season 三处对账）
  - 各子系统平衡：刷新密度<->掉落金、世界事件币、PvP 产出、专业净注入
  - 引用完整性（所有裸 uint32 外键必须落地）
  - 结构校验：区域等级带、门可达性、声望档位单调、XP 曲线单调

退出码：0 全部通过（仅 INFO/WARN 不算失败），1 存在 FAIL。

用法：
  python balance_verifier.py [--data DIR] [--json report.json]
"""
import os
import sys
import json
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "_pb"))

from load_configs import load_all  # noqa: E402
from config_currencies_pb2 import CURRENCY_STATUS_DISABLED  # noqa: E402

try:
    import combat_simulator as _cmb_sim  # noqa: E402
except Exception:  # pragma: no cover
    _cmb_sim = None


# ----------------------------------------------------------------------------
# 报告累加器
# ----------------------------------------------------------------------------
class Report:
    def __init__(self):
        self.lines = []
        self.n_error = 0
        self.n_warn = 0
        self.n_info = 0
        self.n_pass = 0

    def _emit(self, level, tag, msg):
        mark = {"PASS": "✅", "WARN": "⚠️ ", "FAIL": "❌", "INFO": "ℹ️ "}[level]
        self.lines.append(f"  {mark} [{tag}] {msg}")
        {"PASS": "n_pass", "WARN": "n_warn", "FAIL": "n_error", "INFO": "n_info"}[level]
        setattr(self, {"PASS": "n_pass", "WARN": "n_warn", "FAIL": "n_error", "INFO": "n_info"}[level],
                getattr(self, {"PASS": "n_pass", "WARN": "n_warn", "FAIL": "n_error", "INFO": "n_info"}[level]) + 1)

    def pass_(self, tag, msg): self._emit("PASS", tag, msg)
    def warn(self, tag, msg):  self._emit("WARN", tag, msg)
    def fail(self, tag, msg):  self._emit("FAIL", tag, msg)
    def info(self, tag, msg):  self._emit("INFO", tag, msg)
    def section(self, title):
        self.lines.append("")
        self.lines.append(f"=== {title} ===")

    def dump(self, path=None):
        text = "\n".join(self.lines) + "\n"
        if path:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
        return text


def cur_name(n):
    """CurrencyType 枚举序 -> 名称（惰性导入，确保桩已生成）。"""
    import config_balance_pb2  # 由 load_all() -> _ensure_stubs() 先生成
    try:
        return config_balance_pb2.CurrencyType.Name(int(n))
    except Exception:
        return f"#{n}"


def within(a, b, tol):
    if b == 0:
        return abs(a) <= abs(tol)
    return abs(a - b) / abs(b) <= tol


# ----------------------------------------------------------------------------
# 校验主体
# ----------------------------------------------------------------------------
def run(cfgs, rep):
    # 索引集合（引用完整性用）
    cur_ids   = {c.currency_type for c in cfgs["currencies"].currencies} if "currencies" in cfgs else set()
    item_ids  = {i.item_id for i in cfgs["items"].items} if "items" in cfgs else set()
    loot_ids  = {t.loot_table_id for t in cfgs["loot"].tables} if "loot" in cfgs else set()
    cr_ids    = {c.creature_id for c in cfgs["creatures"].creatures} if "creatures" in cfgs else set()
    zone_ids  = {z.zone_id for z in cfgs["zones"].zones} if "zones" in cfgs else set()
    gate_ids  = {g.gate_id for g in cfgs["zones"].gates} if "zones" in cfgs else set()
    fac_ids   = {f.faction_id for f in cfgs["reputation"].factions} if "reputation" in cfgs else set()
    prof_ids  = {p.profession_id for p in cfgs["professions"].professions} if "professions" in cfgs else set()
    season_ids = {s.season_id for s in cfgs["pvp"].seasons} if "pvp" in cfgs else set()
    ev_ids    = {e.event_id for e in cfgs["world_events"].events} if "world_events" in cfgs else set()

    # 便捷反查
    creature_by_id = {c.creature_id: c for c in cfgs["creatures"].creatures} if "creatures" in cfgs else {}
    zone_by_id = {z.zone_id: z for z in cfgs["zones"].zones} if "zones" in cfgs else {}
    currency_by_type = {c.currency_type: c for c in cfgs["currencies"].currencies} if "currencies" in cfgs else {}

    def check_ref(tag, label, rid, idset, target):
        if rid == 0:
            return True
        if rid in idset:
            return True
        rep.fail(tag, f"{label}={rid} 引用了不存在的 {target}（悬空外键）")
        return False

    # ===================================================================
    rep.section("A. 引用完整性 (Referential Integrity)")
    # ---- 生物 -> 掉落表 / 物品 -> 掉落 ----
    if "creatures" in cfgs:
        for c in cfgs["creatures"].creatures:
            check_ref("REF", f"creature {c.creature_id}.loot", c.loot_table_id, loot_ids, "loot_table")
    # ---- 刷新 -> 生物 / 区域 ----
    if "spawns" in cfgs:
        for s in cfgs["spawns"].spawns:
            check_ref("REF", f"spawn {s.spawn_id}.creature", s.creature_template_id, cr_ids, "creature")
            check_ref("REF", f"spawn {s.spawn_id}.zone", s.zone_id, zone_ids, "zone")
    # ---- 商店 -> 物品 / 阵营 ----
    if "vendors" in cfgs:
        for v in cfgs["vendors"].vendors:
            check_ref("REF", f"vendor {v.vendor_id}.faction", v.faction_id, fac_ids, "faction")
            for it in v.items:
                check_ref("REF", f"vendor {v.vendor_id}.item", it.item_id, item_ids, "item")
    # ---- PvP ----
    if "pvp" in cfgs:
        for bg in cfgs["pvp"].battlegrounds:
            check_ref("REF", f"bg {bg.bg_id}.zone", bg.zone_id, zone_ids, "zone")
            check_ref("REF", f"bg {bg.bg_id}.cur", bg.reward_currency_id, cur_ids, "currency")
        for ar in cfgs["pvp"].arenas:
            check_ref("REF", f"arena {ar.arena_id}.zone", ar.zone_id, zone_ids, "zone")
            check_ref("REF", f"arena {ar.arena_id}.season", ar.season_id, season_ids, "season")
        for r in cfgs["pvp"].ranks:
            check_ref("REF", f"rank {r.rank_id}.cur", r.currency_reward_id, cur_ids, "currency")
        for b in cfgs["pvp"].balance:
            check_ref("REF", f"pvp_balance cur", b.currency_id, cur_ids, "currency")
    # ---- 世界事件 ----
    if "world_events" in cfgs:
        for e in cfgs["world_events"].events:
            for z in e.affected_zone_ids:
                check_ref("REF", f"event {e.event_id}.zone", z, zone_ids, "zone")
            for cr in e.creature_ids:
                check_ref("REF", f"event {e.event_id}.creature", cr, cr_ids, "creature")
            check_ref("REF", f"event {e.event_id}.cur", e.event_currency_id, cur_ids, "currency")
    # ---- 声望档位奖励物品 ----
    if "reputation" in cfgs:
        for f in cfgs["reputation"].factions:
            for t in f.tiers:
                for iid in t.reward_item_ids:
                    check_ref("REF", f"faction {f.faction_id}.tier_item", iid, item_ids, "item")
    # ---- 专业 ----
    if "professions" in cfgs:
        for p in cfgs["professions"].professions:
            for g in p.gathers:
                for y in g.yields:
                    check_ref("REF", f"gather {g.node_id}.yield", y.item_id, item_ids, "item")
        for r in cfgs["professions"].recipes:
            check_ref("REF", f"recipe {r.recipe_id}.prof", r.profession_id, prof_ids, "profession")
            for o in r.outputs:
                check_ref("REF", f"recipe {r.recipe_id}.output", o.item_id, item_ids, "item")
            for m in r.materials:
                check_ref("REF", f"recipe {r.recipe_id}.mat", m.item_id, item_ids, "item")
    # ---- 区域/门/实例 ----
    if "zones" in cfgs:
        for g in cfgs["zones"].gates:
            check_ref("REF", f"gate {g.gate_id}.src", g.source_zone_id, zone_ids, "zone")
            check_ref("REF", f"gate {g.gate_id}.dst", g.target_zone_id, zone_ids, "zone")
        for ins in cfgs["zones"].instances:
            check_ref("REF", f"instance {ins.instance_id}.zone", ins.zone_id, zone_ids, "zone")
            check_ref("REF", f"instance {ins.instance_id}.gate", ins.entrance_gate_id, gate_ids, "gate")
            for b in ins.boss_creature_ids:
                check_ref("REF", f"instance {ins.instance_id}.boss", b, cr_ids, "creature")
    rep.pass_("REF", "引用完整性扫描完成" if rep.n_error == 0 else "存在悬空外键，见上")

    # ===================================================================
    rep.section("B. 经济源汇对账 (EconomyFlow — 闭合 GAP-4)")
    if "balance" not in cfgs:
        rep.warn("ECON", "缺少 balance.json，跳过经济源汇校验")
    else:
        flows = cfgs["balance"].economy_flows
        # B1: 每条 flow 的 per_capita_daily 是否落在预期±tolerance
        for fl in flows:
            tol = fl.tolerance_pct or 0.10
            ok = within(fl.per_capita_daily, fl.expected_daily_per_capita, tol)
            tag = f"flow:{fl.flow_id}"
            if ok:
                rep.pass_(tag, f"{cur_name(fl.currency)} {fl.direction} 实测 {fl.per_capita_daily} ≈ 预期 {int(fl.expected_daily_per_capita)} (±{int(tol*100)}%)")
            else:
                rep.fail(tag, f"{cur_name(fl.currency)} {fl.direction} 实测 {fl.per_capita_daily} 偏离预期 {int(fl.expected_daily_per_capita)} 超 {int(tol*100)}%")
            # GAP-6：区分"有意销毁"与"异常泄漏"
            if getattr(fl, "intentional_destroy", False):
                # 拍卖税/销毁费：货币永久退出经济，leak_rate 应≈1.0
                if fl.leak_rate < 0.5:
                    rep.fail(tag, f"intentional_destroy=true 但 leak_rate={fl.leak_rate} 偏低（应≈1.0 表示 100% 销毁），建模矛盾")
                else:
                    rep.pass_(tag, f"intentional_destroy=true（拍卖税/销毁费）：leak_rate={fl.leak_rate} 符合预期（货币永久退出经济）")
            elif fl.leak_rate > 0.02:
                rep.warn(tag, f"leak_rate={fl.leak_rate} 超阈值(>0.02)：货币未经闭环消失，需确认是否为有意销毁(如拍卖税)或为配置笔误")
        # B2: 每币种净流量（源-汇），输出源汇账本
        by_cur = {}
        for fl in flows:
            d = by_cur.setdefault(fl.currency, {"src": 0.0, "snk": 0.0, "n_src": 0, "n_snk": 0})
            if fl.direction == 0:  # FLOW_SOURCE
                d["src"] += fl.per_capita_daily; d["n_src"] += 1
            else:
                d["snk"] += fl.per_capita_daily; d["n_snk"] += 1
        for cur, d in sorted(by_cur.items(), key=lambda x: str(x[0])):
            net = d["src"] - d["snk"]
            if d["src"] + d["snk"] == 0:
                rep.info(cur_name(cur), "无 EconomyFlow 建模（未纳入源汇账本）")
                continue
            if net < 0:
                rep.fail(cur_name(cur), f"净流出 {int(-net)}/人/日（源 {int(d['src'])} / 汇 {int(d['snk'])}）—— 存在通货紧缩/耗尽风险")
            else:
                ratio = net / max(d["src"], 1)
                rep.info(cur_name(cur), f"源 {int(d['src'])} / 汇 {int(d['snk'])} / 净注入 {int(net)} (占比 {ratio*100:.0f}%)")
            # GAP-1 硬门禁：can_lose=false 的币种（不可失去 / 不可销毁）一旦净注入>0，
            # 即永增型通胀、无回收渠道 —— 必须补汇或声明赛季清零(reset)，否则直接 FAIL。
            ccur = currency_by_type.get(cur)
            if ccur is not None and getattr(ccur, "can_lose", True) is False and net > 1e-6:
                rep.fail(cur_name(cur), f"can_lose=false 净注入 {int(net)}/人/日（源 {int(d['src'])} / 汇 {int(d['snk'])}）—— 永增通胀：必须补汇或声明赛季清零(reset)")
        # GAP-3 死币种监管：currencies.json 已声明但无任何 EconomyFlow 的币种
        # - status=DISABLED（有意未启用）-> INFO，跳过监管（不再噪声 WARN）
        # - 默认/ENABLED 却无 EconomyFlow -> WARN（真实遗漏，需补 flow 或明确禁用）
        for c in cfgs["currencies"].currencies:
            if c.currency_type not in by_cur:
                if c.status == CURRENCY_STATUS_DISABLED:
                    rep.info(cur_name(c.currency_type), "已声明但本阶段 DISABLED（有意未启用），跳过死币种监管")
                else:
                    rep.warn(cur_name(c.currency_type), "已声明但无 EconomyFlow 建模（死币种）—— 若本阶段不启用，应在 currencies.json 标记 status:DISABLED")
        # B3: 周上限 vs 单源周潜值（cap 必须可达且不被单源轻易击穿）
        for fl in flows:
            cur = currency_by_type.get(fl.currency)
            if cur is None or cur.weekly_cap == 0:
                continue
            weekly_potential = fl.per_capita_daily * 7
            if weekly_potential > cur.weekly_cap * 1.2:
                rep.warn(cur_name(fl.currency), f"单源 {fl.flow_id} 周潜值 {int(weekly_potential)} 远超周上限 {cur.weekly_cap}（上限形同虚设/源过强）")
            elif weekly_potential <= cur.weekly_cap:
                rep.pass_(cur_name(fl.currency), f"周上限 {cur.weekly_cap} 对单源 {fl.flow_id} 可达且有效")

    # ===================================================================
    rep.section("C. 币种周上限三处对账 (currencies / pvp_balance / season)")
    if "currencies" in cfgs and "pvp" in cfgs:
        for b in cfgs["pvp"].balance:
            cur = currency_by_type.get(b.currency_id)
            if cur is None:
                continue
            if abs(b.weekly_cap - cur.weekly_cap) > 1e-6:
                rep.fail(cur_name(b.currency_id), f"PvpBalance.weekly_cap {int(b.weekly_cap)} ≠ CurrencyConfig.weekly_cap {cur.weekly_cap}")
            else:
                rep.pass_(cur_name(b.currency_id), f"PvpBalance 周上限 {int(b.weekly_cap)} == CurrencyConfig 周上限")
        # season  Conquest 上限对账
        for s in cfgs["pvp"].seasons:
            cc = currency_by_type.get(4)  # CONQUEST
            if cc and abs(s.conquest_cap_weekly - cc.weekly_cap) > 1e-6:
                rep.fail("CONQUEST", f"season {s.season_id} conquest_cap_weekly {s.conquest_cap_weekly} ≠ CurrencyConfig {cc.weekly_cap}")
            elif cc:
                rep.pass_("CONQUEST", f"season {s.season_id} 周上限 {s.conquest_cap_weekly} == CurrencyConfig")
    else:
        rep.warn("CAP", "缺少 currencies/pvp，跳过周上限对账")

    # ===================================================================
    rep.section("D. 刷新密度 <-> 经济源 (掉落金 对账)")
    if "spawns" in cfgs and "creatures" in cfgs and "balance" in cfgs:
        # 每 zone 平均金/击杀
        zone_avg_gold = {}
        for sp in cfgs["spawns"].spawns:
            cr = creature_by_id.get(sp.creature_template_id)
            if cr is None:
                continue
            avg = (cr.gold_min + cr.gold_max) / 2.0
            zone_avg_gold.setdefault(sp.zone_id, []).append(avg)
        expected_gold = 0.0
        for d in cfgs["spawns"].density:
            avgs = zone_avg_gold.get(d.zone_id, [])
            if not avgs:
                rep.warn("DENS", f"zone {d.zone_id} 有密度目标但无刷新点/无金怪")
                continue
            zavg = sum(avgs) / len(avgs)
            expected_gold += d.target_kills_per_player_hour * 24 * zavg
        # 找 mob_drop 金源
        mob = next((f for f in cfgs["balance"].economy_flows
                    if f.currency == 0 and f.source_category == "mob_drop"), None)
        if mob is None:
            rep.warn("DENS", "未找到 mob_drop(GOLD) 的 EconomyFlow，无法对账")
        else:
            if within(expected_gold, mob.expected_daily_per_capita, 0.10):
                rep.pass_("DENS", f"刷新密度推算金源 {int(expected_gold)}/日 ≈ EconomyFlow.mob_drop {int(mob.expected_daily_per_capita)} (±10%)")
            else:
                rep.fail("DENS", f"刷新密度推算金源 {int(expected_gold)}/日 ≠ EconomyFlow.mob_drop {int(mob.expected_daily_per_capita)}（掉率/密度需调）")
    else:
        rep.warn("DENS", "缺少 spawns/creatures/balance，跳过密度对账")

    # ===================================================================
    rep.section("E. 世界事件币注入对账")
    if "world_events" in cfgs and "balance" in cfgs:
        for e in cfgs["world_events"].events:
            if not e.HasField("balance"):
                continue
            daily = e.balance.expected_currency_per_hour * 24
            fl = next((f for f in cfgs["balance"].economy_flows
                       if f.currency == e.event_currency_id and f.source_category == "world_event"), None)
            if fl is None:
                rep.warn("WEVENT", f"event {e.event_id} 声明币 {cur_name(e.event_currency_id)} 注入 {int(daily)}/日，但无对应 EconomyFlow")
                continue
            tol = e.balance.tolerance_pct or 0.10
            if within(daily, fl.expected_daily_per_capita, tol):
                rep.pass_("WEVENT", f"event {e.event_id} 注入 {int(daily)}/日 ≈ EconomyFlow {int(fl.expected_daily_per_capita)} (±{int(tol*100)}%)")
            else:
                rep.fail("WEVENT", f"event {e.event_id} 注入 {int(daily)}/日 ≠ EconomyFlow {int(fl.expected_daily_per_capita)}")
    else:
        rep.warn("WEVENT", "缺少 world_events/balance，跳过")

    # ===================================================================
    rep.section("F. PvP 产出与等级带")
    if "pvp" in cfgs:
        for bg in cfgs["pvp"].battlegrounds:
            z = zone_by_id.get(bg.zone_id)
            if z and z.zone_type != 5:  # BATTLEGROUND
                rep.warn("PVP", f"bg {bg.bg_id} 区域 {bg.zone_id} 类型非 BATTLEGROUND")
            else:
                rep.pass_("PVP", f"bg {bg.bg_id} 绑定战场区域 {bg.zone_id}")
        for ar in cfgs["pvp"].arenas:
            z = zone_by_id.get(ar.zone_id)
            if z and z.zone_type != 6:  # ARENA
                rep.warn("PVP", f"arena {ar.arena_id} 区域 {ar.zone_id} 类型非 ARENA")
            else:
                rep.pass_("PVP", f"arena {ar.arena_id} 绑定竞技场区域 {ar.zone_id}")
    else:
        rep.warn("PVP", "缺少 pvp，跳过")

    # ===================================================================
    rep.section("G. 专业净注入 / 采集速率")
    if "professions" in cfgs:
        for r in cfgs["professions"].recipes:
            if not r.HasField("balance"):
                continue
            b = r.balance
            net = b.output_value_estimate - b.material_cost_estimate
            if net < 0:
                rep.warn("PROF", f"recipe {r.recipe_id} 净注入 {int(net)}（产出<材料，可能为亏本日常/限产）")
            else:
                rep.pass_("PROF", f"recipe {r.recipe_id} 净注入 {int(net)}（产出 {int(b.output_value_estimate)} - 材料 {int(b.material_cost_estimate)}）")
            if b.expected_daily_crafts_per_capita <= 0:
                rep.fail("PROF", f"recipe {r.recipe_id} 人均日产上限为 0")
        for p in cfgs["professions"].professions:
            for g in p.gathers:
                if not g.HasField("balance"):
                    continue
                if g.balance.expected_yields_per_hour <= 0:
                    rep.fail("GATHER", f"node {g.node_id} 预期产量 <= 0")
                else:
                    rep.pass_("GATHER", f"node {g.node_id} 预期 {g.balance.expected_yields_per_hour}/小时")
    else:
        rep.warn("PROF", "缺少 professions，跳过")

    # ===================================================================
    rep.section("H. 区域等级带一致性")
    if "spawns" in cfgs and "zones" in cfgs:
        for sp in cfgs["spawns"].spawns:
            z = zone_by_id.get(sp.zone_id)
            cr = creature_by_id.get(sp.creature_template_id)
            if z is None or cr is None:
                continue
            zmax = z.max_level if z.max_level > 0 else 9999
            overlap = (cr.min_level <= zmax) and (cr.max_level >= z.min_level)
            if overlap:
                rep.pass_("LVBAND", f"spawn {sp.spawn_id}: 生物 {cr.creature_id}({cr.min_level}-{cr.max_level}) ⊂ 区域 {z.zone_id}({z.min_level}-{zmax})")
            else:
                rep.fail("LVBAND", f"spawn {sp.spawn_id}: 生物 {cr.creature_id}({cr.min_level}-{cr.max_level}) 与区域 {z.zone_id}({z.min_level}-{zmax}) 等级带不相交")
    else:
        rep.warn("LVBAND", "缺少 spawns/zones，跳过")

    # ===================================================================
    rep.section("I. 门可达性 (Gate Reachability)")
    if "zones" in cfgs and zone_ids:
        import collections
        adj = collections.defaultdict(set)
        for g in cfgs["zones"].gates:
            adj[g.source_zone_id].add(g.target_zone_id)
            if g.bidirectional:
                adj[g.target_zone_id].add(g.source_zone_id)
        start = min(zone_ids)
        seen = set([start])
        q = [start]
        while q:
            cur = q.pop(0)
            for nxt in adj.get(cur, ()):
                if nxt not in seen:
                    seen.add(nxt); q.append(nxt)
        unreachable = zone_ids - seen
        if not unreachable:
            rep.pass_("GATE", f"全部 {len(zone_ids)} 个区域从区域 {start} 可达（门拓扑连通）")
        else:
            rep.fail("GATE", f"区域 {sorted(unreachable)} 无法通过门到达（孤立区域）")
        # 门进入条件 vs 目标区域等级带
        for g in cfgs["zones"].gates:
            z = zone_by_id.get(g.target_zone_id)
            if z is None:
                continue
            zmax = z.max_level if z.max_level > 0 else 9999
            req_min = g.requirement.min_level if g.HasField("requirement") else 0
            if req_min > zmax:
                rep.fail("GATE", f"gate {g.gate_id}: 进入条件 min_level {req_min} > 目标区域 {z.zone_id} 上限 {zmax}（无人可进）")
    else:
        rep.warn("GATE", "缺少 zones，跳过")

    # ===================================================================
    rep.section("J. 结构单调 / 边界")
    if "reputation" in cfgs:
        for f in cfgs["reputation"].factions:
            ths = [t.threshold for t in f.tiers]
            if ths != sorted(ths):
                rep.fail("REP", f"faction {f.faction_id} 档位阈值非单调: {ths}")
            else:
                rep.pass_("REP", f"faction {f.faction_id} 档位阈值单调: {ths}")
            for s in f.sources:
                if s.daily_cap <= 0:
                    rep.fail("REP", f"faction {f.faction_id} 来源 {s.source_id} daily_cap<=0（可被无限刷）")
    if "balance" in cfgs:
        bal = cfgs["balance"]
        maxlvl = bal.max_level
        pts = [(p.level, p.xp_required) for p in bal.xp_curve]
        levels = sorted(lv for lv, _ in pts)
        xp = [x for _, x in pts]
        # GAP-2 闭合：xp_curve 必须覆盖全部进阶等级 1..(max_level-1)
        #   （XpCurvePoint 语义：level=L 的 xp_required = 升到 L+1 所需经验；
        #    max_level=60 为满级上限，不再进阶，故曲线只需到 59）
        expect = list(range(1, maxlvl))
        if levels == expect and len(set(levels)) == len(levels):
            rep.pass_("XP", f"XP 曲线完整性: 覆盖等级 1..{maxlvl-1}（共 {len(levels)} 条，max_level={maxlvl}）")
        else:
            miss = [l for l in expect if l not in set(levels)]
            rep.fail("XP", f"XP 曲线未覆盖满级: 现有等级 {levels[:3]}..{levels[-3:]}（共 {len(levels)}），应严格为 1..{maxlvl-1}（缺失 {miss[:10]}）")
        # 严格单调升序、无重复
        if xp == sorted(xp) and len(set(xp)) == len(xp):
            rep.pass_("XP", f"XP 曲线单调: 1..{len(xp)} 升序无重复（首 {int(xp[0])} / 末 {int(xp[-1])}）")
        else:
            rep.fail("XP", f"XP 曲线非单调或含重复: {xp}")
        # 单级增量须为正且平滑（>5x 跳变视为配置异常）
        jumps = [i for i in range(1, len(xp)) if xp[i] <= xp[i - 1] or xp[i] > xp[i - 1] * 5]
        if jumps:
            rep.warn("XP", f"XP 曲线存在非递增或跳变>5x 的级索引: {jumps[:10]}")
        else:
            rep.pass_("XP", "XP 曲线单级增量均为正且平滑（无 >5x 跳变）")
    if "stats" in cfgs:
        seen_keys = set()
        dup = False
        for b in cfgs["stats"].budgets:
            k = (b.item_level, b.quality, b.slot)
            if k in seen_keys:
                dup = True
            seen_keys.add(k)
        rep.pass_("STATS", f"物品化预算 {len(cfgs['stats'].budgets)} 条，{'存在重复键!' if dup else '键唯一'}")
        for r in cfgs["stats"].derived_rules:
            if r.input_stat == r.output_stat:
                rep.warn("STATS", f"衍生规则 input==output ({r.input_stat}) 无意义")

    # ===================================================================
    rep.section("K. 战斗强度模拟 (GAP-9)")
    if _cmb_sim is None:
        rep.warn("COMBAT", "combat_simulator 不可用，跳过战斗强度校验")
    elif "balance" not in cfgs or "stats" not in cfgs:
        rep.warn("COMBAT", "缺少 balance/stats，跳过战斗强度校验")
    else:
        res = _cmb_sim.simulate(cfgs)
        for c in res["classes"]:
            tag = f"combat:{_cmb_sim._class_name(c['class'])}"
            mitig = c["mitigation_pct"] / 100.0
            crit = c["crit_pct"] / 100.0
            ok = (c["dps"] > 0 and c["ttk_sec"] not in (None, 0)
                  and _cmb_sim.CONFIG["MITIGATION_MIN"] < mitig < _cmb_sim.CONFIG["MITIGATION_MAX"]
                  and 0.0 <= crit <= 1.0)
            if ok:
                rep.pass_(tag, f"AP={c['attack_power']:.0f} 暴击={c['crit_pct']:.2f}% 裸DPS={c['dps']:.1f} "
                               f"减伤={c['mitigation_pct']:.1f}% 有效DPS={c['eff_dps']:.1f} TTK={c['ttk_sec']:.0f}s（模型自洽）")
            else:
                rep.fail(tag, f"战斗模型输出越界: DPS={c['dps']} mitig={c['mitigation_pct']}% crit={c['crit_pct']}% TTK={c['ttk_sec']}")
        d = res["dispersion"]
        if d["verdict"] == "fail":
            rep.fail("COMBAT", f"职业强度离散度过大: {d['reason']}")
        elif d["verdict"] == "pass":
            rep.pass_("COMBAT", f"职业强度离散度合理: {d['reason']}")
        else:
            rep.info("COMBAT", f"职业强度离散度校验延后: {d['reason']}")

    # ===================================================================
    rep.section("L. 修理成本对账 (GAP-7)")
    # CAMI 装备模型中有耐久的槽位（EquipSlot 枚举值：HEAD=1,CHEST=2,LEGS=3,FEET=4,HANDS=5,MAINHAND=6,OFFHAND=7；
    #   NECK=9/RING=10/TRINKET=8 无耐久，不计入修理成本）
    SLOT_NAME = {0:"SLOT_NONE",1:"SLOT_HEAD",2:"SLOT_CHEST",3:"SLOT_LEGS",4:"SLOT_FEET",
                 5:"SLOT_HANDS",6:"SLOT_MAINHAND",7:"SLOT_OFFHAND",8:"SLOT_TRINKET",9:"SLOT_NECK",10:"SLOT_RING"}
    KNOWN_DURABLE_SLOTS = {1, 2, 3, 4, 5, 6, 7}
    if "balance" not in cfgs:
        rep.warn("REPAIR", "缺少 balance，跳过修理对账")
    else:
        bal = cfgs["balance"]
        rates = bal.repair_rates
        # 结构：每个槽位费率 > 0 且无重复定义
        struct_ok = True
        seen = set()
        for r in rates:
            if r.cost_per_durability <= 0:
                rep.fail("REPAIR", f"槽位 {SLOT_NAME.get(r.slot, r.slot)} cost_per_durability={r.cost_per_durability} 非法（应>0）")
                struct_ok = False
            if r.slot in seen:
                rep.fail("REPAIR", f"槽位 {SLOT_NAME.get(r.slot, r.slot)} 重复定义 repair_rates")
                struct_ok = False
            seen.add(r.slot)
        if struct_ok and rates:
            rep.pass_("REPAIR", f"repair_rates 结构合法：{len(rates)} 个槽位费率均>0 且无重复")
        elif not rates:
            rep.info("REPAIR", "未定义 repair_rates（无修理费率）")
        # 覆盖检查：有耐久槽是否全部建模（缺槽 = 真实缺口，WARN 暴露）
        if rates:
            defined = {r.slot for r in rates}
            missing = sorted(KNOWN_DURABLE_SLOTS - defined)
            if not missing:
                rep.info("REPAIR", f"已建模完整有耐久槽（{len(defined)}/{len(KNOWN_DURABLE_SLOTS)}），对账基于均匀损耗假设；"
                                   f"未来接入真实耐久损耗率 telemetry 后可按槽独立校准")
            else:
                rep.warn("REPAIR", f"有耐久槽未全覆盖（{len(defined)}/{len(KNOWN_DURABLE_SLOTS)}），缺：{', '.join(SLOT_NAME[m] for m in missing)}（GAP-7）")
        # 对账：repair_gold 汇 是否可由 repair_rates × 耐久损耗 解释
        repair_flow = next((f for f in bal.economy_flows
                            if f.source_category == "repair" and f.direction == 1), None)
        if repair_flow is None:
            rep.warn("REPAIR", "无 repair 类汇（repair_gold），无法对账修理成本")
        elif not rates:
            rep.warn("REPAIR", "有 repair_gold 汇但无 repair_rates，成本不可解释")
        else:
            sum_cost = sum(r.cost_per_durability for r in rates)
            implied_loss = repair_flow.per_capita_daily / sum_cost  # 均匀损耗假设下每槽日损耗
            MAX_LOSS = 200.0  # 单槽日耐久损耗合理上限（≈最大耐久，极端情形）
            if implied_loss > MAX_LOSS:
                rep.warn("REPAIR", f"repair_gold 汇 {int(repair_flow.per_capita_daily)}/日 在'均匀损耗'假设下隐含每槽 {implied_loss:.0f} 耐久/日损耗，"
                                   f"远超合理上限({MAX_LOSS:.0f})—— 现有 repair_rates 无法解释该汇，需补充其余槽位与真实耐久损耗率（GAP-7）")
            else:
                rep.pass_("REPAIR", f"repair_gold 汇 {int(repair_flow.per_capita_daily)}/日 可由 repair_rates（Σ费率={sum_cost}）在合理损耗({implied_loss:.0f}/槽/日)内解释")

    # ===================================================================
    rep.section("M. 囤积率上限 (GAP-8)")
    if "balance" not in cfgs:
        rep.warn("HOARD", "缺少 balance，跳过囤积率校验")
    else:
        for fl in cfgs["balance"].economy_flows:
            tag = f"hoard:{fl.flow_id}"
            if fl.hoard_rate > 0.5:
                rep.warn(tag, f"hoard_rate={fl.hoard_rate} 过高（>0.5）：货币过度沉淀，流动性枯竭风险")
            elif fl.leak_rate > 0.02 and fl.hoard_rate > 0.1 and not getattr(fl, "intentional_destroy", False):
                rep.warn(tag, f"同时存在泄漏(leak={fl.leak_rate})与囤积(hoard={fl.hoard_rate})：双重货币退出，需确认是否设计意图")
            else:
                rep.pass_(tag, f"hoard_rate={fl.hoard_rate}（leak={fl.leak_rate}）在合理区间")


# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    default_data = os.path.normpath(os.path.join(HERE, "..", "data", "configs"))
    ap.add_argument("--data", default=default_data)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    rep = Report()
    rep.lines.append("CAMI 平衡校验报告")
    rep.lines.append(f"数据目录: {args.data}")
    cfgs = load_all(args.data)
    rep.info("LOAD", f"已加载 {len(cfgs)} 个 ConfigSet: {', '.join(sorted(cfgs))}")

    run(cfgs, rep)

    rep.section("汇总")
    rep.lines.append(f"  PASS={rep.n_pass}  WARN={rep.n_warn}  FAIL={rep.n_error}  INFO={rep.n_info}")
    text = rep.dump(args.json or None)
    print(text)
    return 1 if rep.n_error > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
