"""CAMI 战斗强度模拟器 (Combat Simulator) —— 闭合 GAP-9

把 `config_balance.CombatConstant` + `config_stats`(预算/衍生规则/职业权重/rating曲线)
落成**可执行的 DPS / TTK 模型**，输出：
  - 每职业(按 class_weights 建模)的 攻击强度(AP)/法术强度(SP)/暴击率/单次伤害/裸 DPS/护甲减伤/有效 DPS/击杀耗时(TTK)
  - 职业强度离散度（多职业时 max/min DPS 比；单职业时显式延后）

伤害路径（架构关键点）：物理职业由 STR/AGI→ATTACK_POWER 驱动、法术职业由
INT→SPELL_POWER 驱动；模拟器按职业"非零衍生力量"自动选 physical(AP×attack_power_coef)
或 spell(SP×spell_power_coef)，两条路径对称、数据驱动（新增职业无需改代码）。
坦克/治疗专精因含 STA→HP 等非输出分配，DPS 天然偏低，**不纳入 DPS 离散度比较**——
本模型仅对"纯 DPS 专精"建模，role 隔离为后续扩展（见 economy-source-sink-map.md GAP-9）。

设计原则（与 balance_verifier 一致）：
  - 仅消费 protobuf 加载后的 `cfgs`，可被 balance_verifier 段 K 直接调用；
  - 所有"配置里没有、模拟必需的常量"（武器基础伤、攻击间隔、目标护甲/血量）集中在
    CONFIG 块并标注 ASSUMPTION —— 它们应最终由 combat.md §1.2 给出权威值；
  - 模型是第一性原理的 white/auto-attack 单目标 PvE 基线，不含技能/dot/多目标；
    技能数值在 Lua 热更边界，不在本离线校验范围。

退出：simulate(cfgs) 返回 dict；__main__ 打印可读表格。
"""
from __future__ import annotations

# ----------------------------------------------------------------------------
# CONFIG —— 配置中缺失、模拟必需的常量（ASSUMPTION，待 combat.md §1.2 权威化）
# ----------------------------------------------------------------------------
CONFIG = {
    "BASE_WEAPON_DMG": 50.0,     # 单次平砍基础伤害（基线装等，待权威化）
    "ATTACK_INTERVAL": 2.0,      # 基础攻击间隔(s)，haste 可压缩（待权威化）
    "TARGET_LEVEL": 60,          # 目标等级（用于护甲减伤公式）
    "TARGET_ARMOR": 2480.0,      # 目标护甲（木桩/Boss 近似，待权威化）
    "TARGET_HP": 100000.0,       # 目标有效血量（TTK 分母，待权威化）
    "CRIT_RATING_ENUM": 10,      # StatType.STAT_CRIT_RATING
    "HASTE_RATING_ENUM": 11,     # StatType.STAT_HASTE_RATING
    # 职业强度离散度硬门禁：max/min DPS 比超过该值即判职业失衡（需 >=2 职业才有意义）
    "DISPERSION_FAIL_RATIO": 1.5,
    # 护甲减伤合理区间（物理减伤不应为负、不应≥95%）
    "MITIGATION_MIN": 0.0,
    "MITIGATION_MAX": 0.95,
}

# StatType 枚举值（来自 common.proto，避免运行期依赖枚举名解析）
STAT_STRENGTH = 3
STAT_STAMINA = 6
STAT_ATTACK_POWER = 8
STAT_SPELL_POWER = 9
STAT_HP = 1


def _rating_lookup(stats_cfg):
    """(level, kind) -> rating_per_percent；kind ∈ {CRIT, HASTE, HIT}。"""
    lut = {}
    for r in stats_cfg.rating_curve:
        kind = {10: "CRIT", 11: "HASTE", 12: "HIT"}.get(int(r.stat))
        if kind:
            lut[(int(r.level), kind)] = float(r.rating_per_percent)
    return lut


def _derived_map(stats_cfg):
    """input_stat -> [(output_stat, conversion), ...]"""
    m = {}
    for d in stats_cfg.derived_rules:
        m.setdefault(int(d.input_stat), []).append((int(d.output_stat), float(d.conversion)))
    return m


def _baseline_budget(stats_cfg):
    """取最具代表性的单件预算作为角色主/次属性代理（数据稀疏时的合理近似）。"""
    if not stats_cfg.budgets:
        return 0, 0
    best = max(stats_cfg.budgets, key=lambda b: (b.primary_budget + b.secondary_budget))
    return int(best.primary_budget), int(best.secondary_budget)


def _mitigation(armor, k, level_adj, level):
    denom = armor + k * (level + level_adj)
    if denom <= 0:
        return 0.0
    return armor / denom


def simulate(cfgs):
    """返回 {assumptions, classes:[...], dispersion:{...}}。"""
    out = {
        "assumptions": dict(CONFIG),
        "classes": [],
        "dispersion": {"ratio": None, "verdict": "deferred", "reason": ""},
    }
    if "balance" not in cfgs or "stats" not in cfgs:
        out["dispersion"]["reason"] = "缺少 balance/stats，跳过战斗模拟"
        return out

    cb = cfgs["balance"].combat
    st = cfgs["stats"]
    rating = _rating_lookup(st)
    derived = _derived_map(st)
    pri_budget, sec_budget = _baseline_budget(st)

    lvl = CONFIG["TARGET_LEVEL"]
    crit_per_pct = rating.get((lvl, "CRIT"), 0.0)  # 每 1% 暴击所需 rating

    for cw in st.class_weights:
        # 1) 主属性预算按职业权重分配
        total_w = sum(max(w.flat, 0) for w in cw.weights) or 1
        prim = {int(w.stat): pri_budget * max(w.flat, 0) / total_w for w in cw.weights}
        # 2) 衍生属性（STR/AGI->AP, INT->SP, STA->HP ...）
        ap = 0.0
        sp = 0.0
        hp = 0.0
        for istat, amt in prim.items():
            for ostat, conv in derived.get(istat, []):
                if ostat == STAT_ATTACK_POWER:
                    ap += amt * conv
                elif ostat == STAT_SPELL_POWER:
                    sp += amt * conv
                elif ostat == STAT_HP:  # STA -> HP 衍生
                    hp += amt * conv
                # 其他衍生输出此处不消费，避免重复累加
        # 3) 暴击率（次属性全部分配到暴击 rating 的保守假设）
        crit_pct = (sec_budget / crit_per_pct / 100.0) if crit_per_pct > 0 else 0.0
        crit_pct = max(0.0, min(crit_pct, 1.0))
        # 4) 武器/法术伤害：物理职业用 AP，法术职业用 SP（取非零衍生力量；两者皆零则仅基础伤）
        if sp > 0:
            power, power_coef, power_kind = sp, cb.spell_power_coef, "spell"
        else:
            power, power_coef, power_kind = ap, cb.attack_power_coef, "physical"
        wdmg = CONFIG["BASE_WEAPON_DMG"] + power * power_coef
        avg_hit = wdmg * (1.0 + crit_pct * (cb.crit_damage_multiplier - 1.0))
        interval = CONFIG["ATTACK_INTERVAL"]
        dps = avg_hit / interval if interval > 0 else 0.0
        # 5) 护甲减伤 / 有效 DPS / TTK
        mitig = _mitigation(CONFIG["TARGET_ARMOR"], cb.armor_mitigation_k,
                            cb.armor_mitigation_level_adj, lvl)
        eff_dps = dps * (1.0 - mitig)
        ttk = (CONFIG["TARGET_HP"] / eff_dps) if eff_dps > 0 else float("inf")

        out["classes"].append({
            "class": int(getattr(cw, "class")),
            "power_stat": power_kind,
            "primary_alloc": {k: round(v, 2) for k, v in prim.items()},
            "attack_power": round(ap, 2),
            "spell_power": round(sp, 2),
            "hp": round(hp, 2),
            "crit_pct": round(crit_pct * 100, 3),
            "weapon_dmg_per_hit": round(wdmg, 2),
            "dps": round(dps, 2),
            "mitigation_pct": round(mitig * 100, 2),
            "eff_dps": round(eff_dps, 2),
            "ttk_sec": round(ttk, 1) if ttk != float("inf") else None,
        })

    # 6) 职业强度离散度
    if len(out["classes"]) >= 2:
        dps_vals = [c["dps"] for c in out["classes"] if c["dps"] > 0]
        if dps_vals:
            ratio = max(dps_vals) / min(dps_vals)
            out["dispersion"] = {
                "ratio": round(ratio, 3),
                "verdict": "fail" if ratio > CONFIG["DISPERSION_FAIL_RATIO"] else "pass",
                "reason": f"max/min DPS 比 {ratio:.3f}（阈值 {CONFIG['DISPERSION_FAIL_RATIO']}）",
            }
    else:
        out["dispersion"]["reason"] = (
            f"仅 {len(out['classes'])} 个职业建模，职业强度离散度校验延后"
            "（需在 stats.json.class_weights 补充更多职业/专精数据）"
        )
    return out


def _class_name(cid):
    try:
        from common_pb2 import ClassType
        return ClassType.Name(int(cid))
    except Exception:
        return f"#{cid}"


def print_report(res):
    print("CAMI 战斗强度模拟报告 (GAP-9)")
    print(f"  假设常量: BASE_WEAPON_DMG={res['assumptions']['BASE_WEAPON_DMG']}  "
          f"ATTACK_INTERVAL={res['assumptions']['ATTACK_INTERVAL']}s  "
          f"TARGET_ARMOR={res['assumptions']['TARGET_ARMOR']}  "
          f"TARGET_HP={res['assumptions']['TARGET_HP']}")
    print("")
    hdr = f"  {'职业':<18}{'AP':>8}{'HP':>8}{'暴击%':>8}{'单伤':>9}{'裸DPS':>10}{'减伤%':>8}{'有效DPS':>10}{'TTK(s)':>10}"
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for c in res["classes"]:
        print(f"  {_class_name(c['class']):<18}{c['attack_power']:>8.1f}{c['hp']:>8.1f}"
              f"{c['crit_pct']:>8.2f}{c['weapon_dmg_per_hit']:>9.1f}{c['dps']:>10.1f}"
              f"{c['mitigation_pct']:>8.1f}{c['eff_dps']:>10.1f}"
              f"{(c['ttk_sec'] if c['ttk_sec'] is not None else 0):>10.0f}")
    d = res["dispersion"]
    print("")
    print(f"  职业强度离散度: {d['verdict'].upper()} —— {d['reason']}")


if __name__ == "__main__":
    import os, sys
    HERE = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, HERE)
    from load_configs import load_all
    cfgs = load_all(os.path.normpath(os.path.join(HERE, "..", "data", "configs")))
    print_report(simulate(cfgs))
