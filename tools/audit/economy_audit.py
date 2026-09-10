#!/usr/bin/env python3
# tools/audit/economy_audit.py — TASK-030 §15.9 / §20.4
#
# 经济账本对账工具：把「账本流水」与「当前余额快照」对起来，输出差异报告。
#
# 对账口径（唯一真相，必须与 C++ 侧完全一致）：
#
#     net(p, c) = Σ{player == p, currency == c} delta  −  Σ{peer == p, currency == c} delta
#
# 为什么要**减去** peer 项：Transfer 每笔命令只写**一行**账本（§7 的 LedgerEntry
# 只有 player / peer 一对），出账方的 delta 是 −amount，入账方那一侧没有独立行。
# 若只累加 `player == p`，收款方的余额变化永远无法被账本解释，对账必然「有差异」。
# C++ 侧对应实现：InMemoryLedgerStore::NetDeltaByPlayer（两边必须同口径）。
#
# 输入文件（由 server/gamenode/economy/tests/ledger_test.cpp 的 §17 集成用例落盘）：
#   bench/economy_ledger_dump.csv  transaction_id,idempotency_key,player,peer,op,currency,
#                                  delta,balance_after,version,timestamp_ms
#   bench/economy_balances.csv     player,currency,balance
#   bench/economy_audit_meta.txt   key=value（ops= / issued= / burned= / total_balance= …）
#
# 退出码：0 = 零差异；1 = 发现差异（§20.4「对账工具零差异」是验收项，必须非零退出）。
#
# 用法：
#   python tools/audit/economy_audit.py                    # 用默认路径
#   python tools/audit/economy_audit.py --ledger <csv> --balances <csv> [--meta <txt>]
#   python tools/audit/economy_audit.py --self-test        # 自检：证明它真的能发现差异
#
# 红线：只读，不改任何文件；不连数据库（离线可跑，§31）。

from __future__ import annotations

import argparse
import csv
import os
import sys

DEFAULT_LEDGER = os.path.join("bench", "economy_ledger_dump.csv")
DEFAULT_BALANCES = os.path.join("bench", "economy_balances.csv")
DEFAULT_META = os.path.join("bench", "economy_audit_meta.txt")

LEDGER_FIELDS = [
    "transaction_id",
    "idempotency_key",
    "player",
    "peer",
    "op",
    "currency",
    "delta",
    "balance_after",
    "version",
    "timestamp_ms",
]


class Finding:
    """一条差异（或统计结论）。"""

    def __init__(self, kind: str, detail: str) -> None:
        self.kind = kind
        self.detail = detail

    def __str__(self) -> str:  # pragma: no cover - 纯展示
        return "[%s] %s" % (self.kind, self.detail)


def _read_ledger(path: str):
    """读账本 CSV；返回 (rows, errors)。行序 = 写入顺序（链与重放都依赖它）。"""
    rows = []
    errors = []
    with open(path, "r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            errors.append(Finding("SCHEMA", "账本 CSV 没有表头"))
            return rows, errors
        missing = [f for f in LEDGER_FIELDS if f not in reader.fieldnames]
        if missing:
            errors.append(Finding("SCHEMA", "账本 CSV 缺列: %s" % ",".join(missing)))
            return rows, errors
        for lineno, raw in enumerate(reader, start=2):
            try:
                rows.append(
                    {
                        "lineno": lineno,
                        "transaction_id": int(raw["transaction_id"]),
                        "idempotency_key": raw["idempotency_key"],
                        "player": int(raw["player"]),
                        "peer": int(raw["peer"]),
                        "op": int(raw["op"]),
                        "currency": int(raw["currency"]),
                        "delta": int(raw["delta"]),
                        "balance_after": int(raw["balance_after"]),
                        "version": int(raw["version"]),
                        "timestamp_ms": int(raw["timestamp_ms"]),
                    }
                )
            except (TypeError, ValueError) as exc:
                errors.append(Finding("PARSE", "第 %d 行解析失败: %s" % (lineno, exc)))
    return rows, errors


def _read_balances(path: str):
    """读余额快照 CSV；返回 (dict[(player,currency)] -> balance, errors)。"""
    balances = {}
    errors = []
    with open(path, "r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None or not {"player", "currency", "balance"}.issubset(
            set(reader.fieldnames)
        ):
            errors.append(Finding("SCHEMA", "余额 CSV 表头必须是 player,currency,balance"))
            return balances, errors
        for lineno, raw in enumerate(reader, start=2):
            try:
                key = (int(raw["player"]), int(raw["currency"]))
                balances[key] = int(raw["balance"])
            except (TypeError, ValueError) as exc:
                errors.append(Finding("PARSE", "余额第 %d 行解析失败: %s" % (lineno, exc)))
    return balances, errors


def _read_meta(path: str):
    meta = {}
    if not path or not os.path.isfile(path):
        return meta
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def reconcile(ledger_rows, balances):
    """核心对账。返回 (findings, stats)。

    四类检查：
      1. 账本内 idempotency_key 必须唯一（§15.3 双保险的「账本侧」体现）；
      2. 逐行重放：按写入顺序把 delta 记到 player，并把 −delta 记到 peer，
         然后在「该行的 player 就是余额归属者」的行上校验 balance_after 是否等于重放值
         —— 这等价于「每个 balance_after 都能被此前的流水解释」；
      3. 逐 (player,currency) 净额 == 余额快照；
      4. 总额守恒：Σ账本净额 == Σ余额快照。
    """
    findings = []
    stats = {
        "rows": len(ledger_rows),
        "duplicates": 0,
        "balance_checks": 0,
        "mismatches": 0,
        "players_in_ledger": 0,
        "players_in_balances": len(balances),
    }

    # ---- 1. 幂等键唯一性 ----
    seen = {}
    for row in ledger_rows:
        key = row["idempotency_key"]
        if key in seen:
            stats["duplicates"] += 1
            findings.append(
                Finding(
                    "DUPLICATE_KEY",
                    "idempotency_key=%s 重复出现（第 %d 行与第 %d 行）—— 账本出现两条同键记录，"
                    "说明 UNIQUE 兜底被绕过" % (key, seen[key], row["lineno"]),
                )
            )
        else:
            seen[key] = row["lineno"]

    # ---- 2. 重放校验 balance_after ----
    running = {}
    for row in ledger_rows:
        cur = row["currency"]
        delta = row["delta"]
        player = row["player"]
        peer = row["peer"]

        if cur != 0:
            rkey = (player, cur)
            running[rkey] = running.get(rkey, 0) + delta
            stats["balance_checks"] += 1
            if running[rkey] != row["balance_after"]:
                stats["mismatches"] += 1
                findings.append(
                    Finding(
                        "REPLAY_MISMATCH",
                        "第 %d 行 txn=%d player=%d cur=%d：重放余额 %d != 记录 balance_after %d"
                        % (
                            row["lineno"],
                            row["transaction_id"],
                            player,
                            cur,
                            running[rkey],
                            row["balance_after"],
                        ),
                    )
                )
            if running[rkey] < 0:
                stats["mismatches"] += 1
                findings.append(
                    Finding(
                        "NEGATIVE_BALANCE",
                        "第 %d 行 player=%d cur=%d 重放后为负（%d）—— §21 禁止负余额"
                        % (row["lineno"], player, cur, running[rkey]),
                    )
                )
            if peer != 0:
                pkey = (peer, cur)
                running[pkey] = running.get(pkey, 0) - delta

    # ---- 3. 逐玩家净额 vs 余额快照 ----
    net = {}
    for row in ledger_rows:
        cur = row["currency"]
        if cur == 0:
            continue
        net[(row["player"], cur)] = net.get((row["player"], cur), 0) + row["delta"]
        if row["peer"] != 0:
            pkey = (row["peer"], cur)
            net[pkey] = net.get(pkey, 0) - row["delta"]

    stats["players_in_ledger"] = len({k[0] for k in net})

    for key, bal in sorted(balances.items()):
        got = net.get(key, 0)
        if got != bal:
            stats["mismatches"] += 1
            findings.append(
                Finding(
                    "NET_VS_BALANCE",
                    "player=%d currency=%d：账本净额 %d != 当前余额 %d（差异 %+d）"
                    % (key[0], key[1], got, bal, bal - got),
                )
            )

    for key in sorted(set(net) - set(balances)):
        findings.append(
            Finding(
                "MISSING_BALANCE",
                "player=%d currency=%d 在账本里有流水但余额快照里没有 —— 可能是对手方"
                "（Transfer 收款人）未纳入快照，需人工确认" % (key[0], key[1]),
            )
        )
    for key in sorted(set(balances) - set(net)):
        if balances[key] != 0:
            stats["mismatches"] += 1
            findings.append(
                Finding(
                    "MISSING_LEDGER",
                    "player=%d currency=%d 余额为 %d 但账本里没有任何流水"
                    % (key[0], key[1], balances[key]),
                )
            )

    # ---- 4. 总额守恒 ----
    stats["total_net"] = sum(net.values())
    stats["total_balance"] = sum(balances.values())
    if stats["total_net"] != stats["total_balance"]:
        stats["mismatches"] += 1
        findings.append(
            Finding(
                "CONSERVATION",
                "Σ账本净额 %d != Σ余额快照 %d" % (stats["total_net"], stats["total_balance"]),
            )
        )

    return findings, stats, net


def _print_report(findings, stats, meta, ledger_path, balances_path):
    print("== TASK-030 经济账本对账 ==")
    print("ledger   : %s" % ledger_path)
    print("balances : %s" % balances_path)
    if meta:
        for k in ("ops", "players", "applied", "rejected", "ledger_rows", "issued", "burned",
                  "total_balance"):
            if k in meta:
                print("meta.%-14s: %s" % (k, meta[k]))
        # 与 meta 交叉校验：落盘的行数、余额合计必须自洽
        if "ledger_rows" in meta and int(meta["ledger_rows"]) != stats["rows"]:
            print(
                "  !! meta.ledger_rows=%s != CSV 行数 %d"
                % (meta["ledger_rows"], stats["rows"])
            )
    print()
    print("-- 统计 --")
    print("rows                = %d" % stats["rows"])
    print("duplicate_keys      = %d  (期望 0)" % stats["duplicates"])
    print("balance_after_checks= %d" % stats["balance_checks"])
    print("players(账本/快照)   = %d / %d" % (stats["players_in_ledger"],
                                              stats["players_in_balances"]))
    print("sum_delta_net       = %d" % stats["total_net"])
    print("sum_balances        = %d" % stats["total_balance"])
    print("mismatches          = %d" % stats["mismatches"])
    print()
    if findings:
        print("-- 差异明细（%d 条）--" % len(findings))
        for f in findings:
            print("  %s" % f)
        print()
        print("RESULT: MISMATCH")
    else:
        print("RESULT: OK — 账本可完整解释每一个余额，零差异（§20.4）")


def run(ledger_path, balances_path, meta_path):
    findings = []
    for path, what in ((ledger_path, "账本"), (balances_path, "余额快照")):
        if not os.path.isfile(path):
            print("ERROR: %s文件不存在: %s" % (what, path))
            print("提示：先跑 server/gamenode/economy/tests/ledger_test.cpp 的 §17 集成用例，"
                  "它会把 CSV 落到 bench/ 下。")
            return 2

    ledger_rows, errs = _read_ledger(ledger_path)
    findings.extend(errs)
    balances, errs2 = _read_balances(balances_path)
    findings.extend(errs2)
    meta = _read_meta(meta_path)

    if not ledger_rows:
        findings.append(Finding("EMPTY", "账本 CSV 没有任何数据行"))

    sub, stats, _net = reconcile(ledger_rows, balances)
    findings.extend(sub)

    _print_report(findings, stats, meta, ledger_path, balances_path)
    return 0 if not findings else 1


# --------------------------------------------------------------------------- 自检

def self_test():
    """证明本工具**真的能发现差异**（否则「零差异」只是没检查的另一种说法）。

    构造一段 3 行的迷你账本（发行 / 转账 / 销毁），先在干净数据上必须零差异，
    再分别注入三类故障，各自都必须被检出。
    """
    print("== economy_audit.py 自检 ==")
    ok = True

    def ledger_rows(spec):
        return [
            {
                "lineno": i + 2,
                "transaction_id": d[0],
                "idempotency_key": d[1],
                "player": d[2],
                "peer": d[3],
                "op": d[4],
                "currency": d[5],
                "delta": d[6],
                "balance_after": d[7],
                "version": d[8],
                "timestamp_ms": d[9],
            }
            for i, d in enumerate(spec)
        ]

    # (txn, key, player, peer, op, cur, delta, balance_after, version, ts)
    clean_spec = [
        (1, "init:1", 100, 0, 0, 1, 1000, 1000, 1, 1),   # 发行给 100
        (2, "init:2", 200, 0, 0, 1, 500, 500, 1, 2),     # 发行给 200
        (3, "mix:1", 100, 200, 2, 1, -300, 700, 2, 3),   # 100 → 200 转 300
        (4, "mix:2", 200, 0, 1, 1, -100, 700, 2, 4),     # 200 销毁 100
    ]
    clean_bal = {(100, 1): 700, (200, 1): 700}

    def check(name, spec, bal, expect_findings):
        nonlocal ok
        f, _s, _n = reconcile(ledger_rows(spec), bal)
        kinds = sorted({x.kind for x in f})
        hit = bool(f) == expect_findings
        ok = ok and hit
        print("  %-28s findings=%-2d kinds=%s  -> %s"
              % (name, len(f), kinds if kinds else "-", "PASS" if hit else "FAIL"))

    check("干净数据（期望零差异）", clean_spec, clean_bal, False)

    tampered = [list(r) for r in clean_spec]
    tampered[2][6] = -3000  # 偷改 delta
    check("篡改 delta", [tuple(r) for r in tampered], clean_bal, True)

    dup = [list(r) for r in clean_spec]
    dup[2][1] = "init:1"  # 复用已存在的幂等键
    check("重复 idempotency_key", [tuple(r) for r in dup], clean_bal, True)

    check("余额被多算", clean_spec, {(100, 1): 7000, (200, 1): 700}, True)

    print()
    print("RESULT: %s" % ("OK — 对账工具可检出差异" if ok else "FAIL — 对账工具存在盲区"))
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(description="CAMI 经济账本对账工具（TASK-030 §15.9）")
    ap.add_argument("--ledger", default=DEFAULT_LEDGER, help="账本 CSV")
    ap.add_argument("--balances", default=DEFAULT_BALANCES, help="余额快照 CSV")
    ap.add_argument("--meta", default=DEFAULT_META, help="落盘元信息 txt（可选）")
    ap.add_argument("--self-test", action="store_true", help="自检：证明能发现差异")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    return run(args.ledger, args.balances, args.meta)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
