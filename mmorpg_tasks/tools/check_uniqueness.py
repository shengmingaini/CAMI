# -*- coding: utf-8 -*-
"""统计 39 份任务规格中各章节的「唯一内容数」，用于量化模板化程度。

用法：python tools/check_uniqueness.py
判定：唯一数 / 39 越接近 1 越好；= 1 表示该章节全库雷同（模板套话）。
"""
import os
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.dirname(HERE)
TASKS_DIR = os.path.join(PACK, "tasks")

files = sorted(f for f in os.listdir(TASKS_DIR) if re.match(r"^TASK-\d{3}\.md$", f))
docs = {}
for fn in files:
    txt = open(os.path.join(TASKS_DIR, fn), encoding="utf-8").read()
    parts = re.split(r"^## ", txt, flags=re.M)[1:]
    sec = {}
    for p in parts:
        lines = p.split("\n")
        title = lines[0].strip()
        body = "\n".join(lines[1:]).strip()
        body = re.sub(r"\s+", " ", body)
        sec[title] = body
    docs[fn] = sec

titles = []
for fn in files:
    for t in docs[fn]:
        if t not in titles:
            titles.append(t)

print("| 章节 | 唯一内容数 / %d | 判定 |" % len(files))
print("|---|---|---|")
bad = 0
for t in titles:
    vals = set()
    for fn in files:
        if t in docs[fn]:
            vals.add(docs[fn][t])
    n = len(vals)
    verdict = "全库雷同" if n == 1 else ("疑似模板" if n <= 3 else "有信息量")
    if n == 1:
        bad += 1
    print("| %s | %d | %s |" % (t, n, verdict))
print()
print("雷同章节数：%d / %d" % (bad, len(titles)))

# 关键词覆盖
KW = ["MessageEnvelope", "FlatBuffers", "Tick Safe Point", "IdempotencyKey",
      "shard", "50000", "4 核", "4核", "ctest", "scripts/verify",
      "require_tasks_done", "STATUS", "git push", "task-done.sh"]
alltxt = "\n".join(open(os.path.join(TASKS_DIR, f), encoding="utf-8").read() for f in files)
print()
print("关键词命中次数（全库 39 份合计）：")
for k in KW:
    print("  %-22s %d" % (k, alltxt.count(k)))
