#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""parse_trace.py —— 按 TraceID 串联一次请求跨线程 / 跨模块的全部日志（TASK-002 §23）。

背景
----
MMO 服务端一条请求会在网关、战斗线程、数据线程之间流转，日志由各线程各自的
`thread_local LogContext` 打点。唯一的串联钥匙是 `TraceID`（uint64，
`(node_id << 48) | (timestamp_low << 16) | counter`，单调递增、可排序、非随机）。

本脚本只做一件事：给定一个 TraceID，从日志文件中抽出全部相关行，按时间排序输出。

用法
----
    python tools/logtrace/parse_trace.py <trace_id> [文件/目录 ...]
    python tools/logtrace/parse_trace.py 0001000200030004 bench/app.log
    python tools/logtrace/parse_trace.py 0x0001000200030004 --dir bench
    python tools/logtrace/parse_trace.py 0001000200030004          # 默认扫描 bench/ 与 logs/

参数
----
    trace_id      16 位十六进制（大小写不敏感），可带 0x / trace= 前缀；
                  也可给低 16 位前缀做模糊匹配（见 --fuzzy）。
    paths         日志文件或目录（目录递归扫描 *.log*）。省略时默认
                  扫描 bench/ 与 logs/（存在的话）。
    --dir DIR     额外指定一个目录（可重复）。
    --fuzzy       允许 trace 后缀/前缀匹配（用于只记得低位的场景）。
    --json        输出 JSON 数组而非原始行（便于二次处理）。
    --context N   每条命中行前后多打印 N 行上下文（默认 0）。
    --quiet       只统计不打印正文。

支持的日志行格式
----------------
1) 文本模式（`LoggerConfig.json = false`，默认）：

    1972-02-29T00:00:00.000000000Z WARN svc=gamenode mod=skill trace=0001000200030004 \
        req=0001000200030005 player=42 scene=7 tid=3 msg=cast skill 17

2) JSON 模式（`LoggerConfig.json = true`）：

    {"ts_ns":...,"ts":"...","level":"WARN","service":"gamenode","module":"skill",
     "trace_id":"0001000200030004","request_id":"...","player_id":42,"scene_id":7,
     "thread_id":3,"message":"cast skill 17"}

排序规则
--------
文本模式按行首 ISO-8601 定宽时间戳排序（字典序 == 时间序）；
JSON 模式按 `ts_ns` 数值排序。同一时刻的多条日志保持文件中的相对顺序（稳定排序）。

退出码
------
    0  至少命中一条
    1  未命中任何行（TraceID 写错，或日志已过期/被滚动删除）
    2  参数错误（TraceID 非法、文件不存在等）
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Iterable, List, Optional, Tuple

# 文本模式：trace=<16 hex>
RE_TEXT_TRACE = re.compile(r"\btrace=([0-9a-fA-F]{1,16})\b")
# JSON 模式："trace_id":"<16 hex>"
RE_JSON_TRACE = re.compile(r'"trace_id"\s*:\s*"([0-9a-fA-F]{1,16})"')
# JSON 模式的 ts_ns（用于排序）
RE_JSON_TS = re.compile(r'"ts_ns"\s*:\s*(-?\d+)')
# 文本模式行首时间戳：1970-01-01T00:00:00.000000000Z
RE_TEXT_TS = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z)")

DEFAULT_DIRS = ("bench", "logs")
LOG_SUFFIXES = (".log", ".log.1", ".log.2", ".log.3", ".log.4", ".log.5",
                ".log.6", ".log.7", ".log.8", ".log.9")


class TraceParseError(Exception):
    """参数或输入错误。"""


def normalize_trace(raw: str) -> str:
    """把用户输入的 TraceID 规整成小写、无前缀的十六进制串。"""
    s = raw.strip().lower()
    if s.startswith("0x"):
        s = s[2:]
    if s.startswith("trace="):
        s = s[len("trace="):]
    # 允许 8 位（低 48 位写法）或 16 位（完整 uint64）
    if not s:
        raise TraceParseError("TraceID 为空")
    if not re.fullmatch(r"[0-9a-f]{1,16}", s):
        raise TraceParseError(
            f"TraceID 非法：{raw!r}（应为 1~16 位十六进制，例如 0001000200030004）")
    return s


def iter_log_files(paths: Iterable[str]) -> List[str]:
    """展开用户给的文件/目录，返回去重后的日志文件列表（保持顺序）。"""
    found: List[str] = []
    seen = set()

    def add(p: str) -> None:
        rp = os.path.normpath(p)
        if rp in seen:
            return
        seen.add(rp)
        found.append(rp)

    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for name in sorted(files):
                    if name.endswith(LOG_SUFFIXES) or name.endswith(".log.json"):
                        add(os.path.join(root, name))
        elif os.path.isfile(p):
            add(p)
        else:
            raise TraceParseError(f"路径不存在：{p}")
    return found


def default_files() -> List[str]:
    """未显式给路径时的默认扫描集合：bench/ 与 logs/（存在才算）。"""
    return iter_log_files([d for d in DEFAULT_DIRS if os.path.isdir(d)])


def read_lines(path: str) -> List[str]:
    """读取日志文件；忽略无法解码的坏行（滚动文件可能被截断）。"""
    out: List[str] = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            out.append(line.rstrip("\r\n"))
    return out


def trace_of_line(line: str) -> Optional[str]:
    """抽取一行的 TraceID；无则返回 None。"""
    m = RE_JSON_TRACE.search(line)
    if m:
        return m.group(1).lower()
    m = RE_TEXT_TRACE.search(line)
    if m:
        return m.group(1).lower()
    return None


def sort_key_of_line(line: str) -> Tuple[str, object]:
    """排序键。文本模式取行首 ISO 时间戳，JSON 模式取 ts_ns 数值。

    首元素为「格式类别」，保证跨格式的混合日志也不会出现 str/int 比较错误；
    解析不出时间的行（如多行堆栈）排到最后。同键时依赖 Python 稳定排序，
    维持文件内的相对出现顺序。
    """
    m = RE_JSON_TS.search(line)
    if m:
        return ("1", int(m.group(1)))
    m = RE_TEXT_TS.match(line)
    if m:
        return ("0", m.group(1))
    return ("9", "")


def collect(trace: str, files: List[str], fuzzy: bool) -> List[Tuple[str, int, str]]:
    """扫描所有文件，收集命中行。返回 (文件, 行号, 行内容) 列表，按时间排序。"""
    hits: List[Tuple[Tuple[str, object], Tuple[str, int, str]]] = []
    for path in files:
        for lineno, line in enumerate(read_lines(path), start=1):
            got = trace_of_line(line)
            if got is None:
                continue
            if fuzzy:
                matched = got.endswith(trace) or trace.endswith(got)
            else:
                # 短写（如只给低 8 位）按后缀匹配，完整 16 位按全等匹配
                matched = got == trace if len(trace) == 16 else got.endswith(trace)
            if not matched:
                continue
            hits.append((sort_key_of_line(line), (path, lineno, line)))
    # Python 的 sort 稳定：时间相同的多条日志保持文件内相对顺序
    hits.sort(key=lambda item: item[0])
    return [item[1] for item in hits]


def print_hits(hits: List[Tuple[str, int, str]], files: List[str],
               as_json: bool, context: int, quiet: bool) -> None:
    """输出命中结果。context>0 时补打印前后 N 行（需按文件重新读取）。"""
    if as_json:
        payload = []
        for path, lineno, line in hits:
            item = {"file": path, "line": lineno, "raw": line}
            m = RE_JSON_TS.search(line)
            if m:
                item["ts_ns"] = int(m.group(1))
            m = RE_TEXT_TS.match(line)
            if m:
                item["ts"] = m.group(1)
            m = RE_JSON_TRACE.search(line) or RE_TEXT_TRACE.search(line)
            if m:
                item["trace_id"] = m.group(1).lower()
            payload.append(item)
        sys.stdout.write(json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    elif not quiet:
        if context > 0:
            cache: dict = {}
            for path in {h[0] for h in hits}:
                cache[path] = read_lines(path)
            last_end: dict = {}
            for path, lineno, line in hits:
                start = max(1, lineno - context)
                if last_end.get(path, 0) < start:
                    sys.stdout.write(f"---- {path}:{lineno} ----\n")
                for i in range(start, min(len(cache[path]), lineno + context) + 1):
                    marker = ">" if i == lineno else " "
                    sys.stdout.write(f"{marker} {cache[path][i - 1]}\n")
                last_end[path] = lineno + context
        else:
            for path, lineno, line in hits:
                sys.stdout.write(f"{line}\n")

    # 摘要走 stderr，不污染可被管道消费的 stdout
    sys.stderr.write(
        f"[parse_trace] hit {len(hits)} line(s) in {len(files)} file(s)\n")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="parse_trace.py",
        description="按 TraceID 抽出并排序一次请求的全部日志（TASK-002）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="示例：python tools/logtrace/parse_trace.py 0001000200030004 bench/app.log",
    )
    p.add_argument("trace", help="TraceID（16 位十六进制，可带 0x / trace= 前缀）")
    p.add_argument("paths", nargs="*", help="日志文件或目录；省略时扫描 bench/ 与 logs/")
    p.add_argument("--dir", action="append", default=[], metavar="DIR",
                   help="额外扫描目录（可重复）")
    p.add_argument("--fuzzy", action="store_true",
                   help="允许前缀/后缀模糊匹配（只记得 TraceID 片段时用）")
    p.add_argument("--json", action="store_true", dest="as_json",
                   help="输出 JSON 数组而非原始行")
    p.add_argument("--context", type=int, default=0, metavar="N",
                   help="每条命中行前后多打印 N 行上下文（默认 0）")
    p.add_argument("--quiet", action="store_true", help="只输出统计，不打印正文")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        trace = normalize_trace(args.trace)
        paths = list(args.paths) + list(args.dir)
        files = iter_log_files(paths) if paths else default_files()
    except TraceParseError as e:
        sys.stderr.write(f"[parse_trace] 参数错误：{e}\n")
        return 2

    if not files:
        sys.stderr.write(
            "[parse_trace] 未找到任何日志文件。"
            "请显式给出路径，或确认 bench/ 与 logs/ 下存在 *.log\n")
        return 2

    hits = collect(trace, files, args.fuzzy)
    print_hits(hits, files, args.as_json, args.context, args.quiet)
    return 0 if hits else 1


if __name__ == "__main__":
    sys.exit(main())
