#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/report/compare.py — CAMI Combat Benchmark 趋势对比工具

解析 combat-report 的底层数据（matrix.json）并生成趋势对比：
  - 单文件：以 Markdown 表格打印完整 20 组矩阵（含八阶段 P95）。
  - 双文件：逐组对比 tick_avg/p50/p95/p99 与资源指标（RSS/CPU/消息量），
            标记回归（P95 上升超过 --regress 阈值，或新值突破验收线）。

数据源约定（与 benchmark/combat 导出一致）：
  - docs/benchmark/matrix.json 为完整矩阵（含 phase_p95_us 对象、cpu_percent、msgs_out_per_sec）。
  - bench/combat_matrix.json 为精简聚合（仅核心 tick 字段），本工具同样兼容。

用法：
  python3 tools/report/compare.py [A.json]                 # 打印 A 的完整矩阵
  python3 tools/report/compare.py A.json B.json           # A->B 趋势对比
  python3 tools/report/compare.py A.json B.json --md      # 输出 Markdown 而非表格文本
  python3 tools/report/compare.py A.json --check 1000     # 仅校验 1000 玩家最差场景是否达线

零第三方依赖（仅标准库）。
"""

import argparse
import json
import sys
from pathlib import Path

# 验收线（与 TASK-025 §8 / §20 对齐）
PASS_LINE = {"player_count": 1000, "tick_avg_us_lt": 5000, "tick_p95_us_le": 5000, "tick_p99_us_le": 8000}

PHASES = ["Input", "Movement", "AOI", "Combat", "Buff", "Quest", "Event", "Replication"]


def _load(path: str) -> dict:
    p = Path(path)
    if not p.exists():
        sys.exit(f"[compare] 文件不存在: {path}")
    try:
        obj = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        sys.exit(f"[compare] JSON 解析失败 {path}: {e}")
    # 兼容两种包装：顶层为 {"scenarios":[...]} 或裸数组
    if isinstance(obj, list):
        return {"scenarios": obj}
    if "scenarios" in obj:
        return obj
    # 单场景对象
    return {"scenarios": [obj]}


def _scenarios(obj: dict) -> list:
    return obj.get("scenarios", [])


def _phase_dict(r: dict) -> dict:
    """归一化 phase_p95_us：可能内联在对象里，也可能平铺为 phase_p95_us_<Phase>。"""
    d = r.get("phase_p95_us")
    if isinstance(d, dict):
        return {k: int(v) for k, v in d.items()}
    out = {}
    for ph in PHASES:
        key = f"phase_p95_us_{ph}"
        if key in r:
            out[ph] = int(r[key])
    return out


def _fmt(v) -> str:
    if isinstance(v, float):
        return f"{v:.2f}"
    return str(v)


def _pass_flag(r: dict) -> str:
    """对 1000 玩家最差场景（Combat100）做达线判定；非 1000 组不参与硬判定。"""
    if int(r.get("player_count", 0)) != PASS_LINE["player_count"]:
        return ""
    avg = int(r.get("tick_avg_us", 0))
    p95 = int(r.get("tick_p95_us", 0))
    p99 = int(r.get("tick_p99_us", 0))
    ok = (avg < PASS_LINE["tick_avg_us_lt"] and p95 <= PASS_LINE["tick_p95_us_le"]
          and p99 <= PASS_LINE["tick_p99_us_le"])
    return "OK " if ok else "FAIL"


def print_matrix(obj: dict, markdown: bool = False) -> None:
    rows = _scenarios(obj)
    if not rows:
        print("[compare] 无场景数据")
        return
    hdr = ["规模", "场景", "Avg", "P50", "P95", "P99", "Max", "AOI可见", "战斗事件/s", "RSS(MB)", "CPU(%)"]
    if markdown:
        print("| " + " | ".join(hdr) + " |")
        print("|" + "|".join(["---"] * len(hdr)) + "|")
    for r in rows:
        line = [
            str(r.get("player_count", "")),
            str(r.get("name", "")),
            _fmt(r.get("tick_avg_us", "")),
            _fmt(r.get("tick_p50_us", "")),
            _fmt(r.get("tick_p95_us", "")),
            _fmt(r.get("tick_p99_us", "")),
            _fmt(r.get("tick_max_us", "")),
            _fmt(r.get("aoi_avg_visible", "")),
            _fmt(r.get("combat_events_per_sec", "")),
            _fmt(r.get("peak_rss_mb", "")),
            _fmt(r.get("cpu_percent", "")),
        ]
        flag = _pass_flag(r)
        if flag:
            line[-1] = f"{line[-1]} {flag}"
        if markdown:
            print("| " + " | ".join(line) + " |")
        else:
            print("  ".join(f"{c:<14}" for c in line))
    # 八阶段 P95 分解
    if markdown:
        print("\n### 八阶段 P95 分解（µs）\n")
        print("| 场景 | " + " | ".join(PHASES) + " |")
        print("|---|" + "|".join(["---"] * len(PHASES)) + "|")
    else:
        print("\n八阶段 P95 分解（µs）:")
    for r in rows:
        pd = _phase_dict(r)
        if not pd:
            continue
        cells = [str(r.get("name", ""))] + [_fmt(pd.get(ph, 0)) for ph in PHASES]
        if markdown:
            print("| " + " | ".join(cells) + " |")
        else:
            print("  ".join(f"{c:<14}" for c in cells))


def diff_matrix(a: dict, b: dict, markdown: bool, regress: float) -> None:
    sa = {r.get("name"): r for r in _scenarios(a)}
    sb = {r.get("name"): r for r in _scenarios(b)}
    names = list(sa.keys())
    missing = [n for n in names if n not in sb]
    hdr = ["场景", "AvgΔ", "P95Δ", "P99Δ", "RSSΔ", "CPUΔ", "回归?"]
    if markdown:
        print("| " + " | ".join(hdr) + " |")
        print("|" + "|".join(["---"] * len(hdr)) + "|")
    else:
        print("趋势对比 A->B（Δ = B - A，单位 µs / MB / %）：")
        print("  ".join(f"{c:<16}" for c in hdr))
    any_regress = False
    for n in names:
        ra, rb = sa[n], sb.get(n, {})
        if n not in sb:
            continue
        def d(k):
            return int(rb.get(k, 0)) - int(ra.get(k, 0))
        davg, dp95, dp99 = d("tick_avg_us"), d("tick_p95_us"), d("tick_p99_us")
        drss = int(rb.get("peak_rss_mb", 0)) - int(ra.get("peak_rss_mb", 0))
        dcpu = round(float(rb.get("cpu_percent", 0)) - float(ra.get("cpu_percent", 0)), 2)
        # 回归判定：P95 上升比例超过阈值，或 1000 组突破验收线
        regr = ""
        base_p95 = int(ra.get("tick_p95_us", 0))
        if base_p95 > 0 and dp95 / base_p95 * 100.0 > regress:
            regr = "REGRESS"
            any_regress = True
        if int(rb.get("player_count", 0)) == PASS_LINE["player_count"] and float(rb.get("combat_ratio", 0)) == 1.0:
            if (int(rb.get("tick_avg_us", 0)) >= PASS_LINE["tick_avg_us_lt"]
                    or int(rb.get("tick_p95_us", 0)) > PASS_LINE["tick_p95_us_le"]
                    or int(rb.get("tick_p99_us", 0)) > PASS_LINE["tick_p99_us_le"]):
                regr = "OVER_LINE"
                any_regress = True
        cells = [n, f"{davg:+d}", f"{dp95:+d}", f"{dp99:+d}", f"{drss:+d}", f"{dcpu:+.2f}", regr]
        if markdown:
            print("| " + " | ".join(cells) + " |")
        else:
            print("  ".join(f"{c:<16}" for c in cells))
    if missing:
        print(f"[compare] B 缺失组：{missing}")
    print(f"[compare] 回归摘要：{'发现回归' if any_regress else '无回归'}")


def check_only(obj: dict, target_pc: int) -> int:
    # 绑定门禁 = 1000 玩家 100% Combat（与 task-025.sh 断言 combat_1000_100pct.txt 一致）
    rows = [r for r in _scenarios(obj)
            if int(r.get("player_count", 0)) == target_pc and float(r.get("combat_ratio", 0)) == 1.0]
    if not rows:
        print(f"[compare] 无 {target_pc} 玩家 100% Combat 组数据")
        return 2
    gate = rows[0]
    avg = int(gate.get("tick_avg_us", 0))
    p95 = int(gate.get("tick_p95_us", 0))
    p99 = int(gate.get("tick_p99_us", 0))
    ok = (avg < PASS_LINE["tick_avg_us_lt"] and p95 <= PASS_LINE["tick_p95_us_le"]
          and p99 <= PASS_LINE["tick_p99_us_le"])
    print(f"[compare] {target_pc} 玩家绑定门禁（100% Combat）= {gate.get('name')} "
          f"(avg={avg} p95={p95} p99={p99}) -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="CAMI Combat Benchmark 趋势对比")
    ap.add_argument("a", help="矩阵 JSON 文件 A（baseline 或单文件展示）")
    ap.add_argument("b", nargs="?", help="矩阵 JSON 文件 B（对比目标）")
    ap.add_argument("--md", action="store_true", help="Markdown 输出")
    ap.add_argument("--regress", type=float, default=5.0, help="P95 上升触发回归标记的百分比阈值（默认 5）")
    ap.add_argument("--check", type=int, default=0, help="仅校验指定规模玩家的最差场景是否达线（如 1000）")
    args = ap.parse_args()

    obj_a = _load(args.a)
    if args.check:
        return check_only(obj_a, args.check)
    if args.b:
        obj_b = _load(args.b)
        diff_matrix(obj_a, obj_b, args.md, args.regress)
        return 0
    print_matrix(obj_a, args.md)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
