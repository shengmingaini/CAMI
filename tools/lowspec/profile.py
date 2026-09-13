#!/usr/bin/env python3
"""TASK-036 低配实测脚本 —— 采集 RAM / VRAM / FPS / DrawCall 曲线并出文本报告。

用法：
    python3 tools/lowspec/profile.py [--quality low|medium|high]
                                     [--duration N] [--bench <path-to-resource_bench>]
                                     [--out <report.txt>]

行为：
    1. 调用 resource_bench（默认 bin/resource_bench，相对仓库根运行），实时解析其
       SAMPLE 行与最终 key=value 汇总行；
    2. 计算 RAM / VRAM / DrawCall / FPS 的 min/avg/max 与阈值（ram<=1536 / vram<=1024
       / draw_calls<=300）；
    3. 输出文本报告（含极简 ASCII 曲线），可选写 --out。

不依赖任何第三方库，仅用标准库。
"""

import argparse
import os
import statistics
import subprocess
import sys


def parse_args():
    p = argparse.ArgumentParser(description="TASK-036 low-spec profiler")
    p.add_argument("--quality", default="low", choices=["low", "medium", "high"])
    p.add_argument("--duration", type=int, default=60)
    p.add_argument("--bench", default="bin/resource_bench",
                   help="resource_bench 可执行路径（相对仓库根）")
    p.add_argument("--out", default="", help="把报告额外写入该文件")
    return p.parse_args()


def run_bench(bench, quality, duration):
    """运行 benchmark，返回 (samples, summary)。samples 为 dict 列表，summary 为最终 key=value。"""
    cmd = [bench, "--quality", quality, "--duration", str(duration)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    except FileNotFoundError:
        sys.stderr.write("无法执行 benchmark：%s\n" % bench)
        sys.stderr.write("请先构建 resource_bench（例如 cmake --build ... --target resource_bench）\n")
        sys.exit(2)

    samples = []
    summary = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.startswith("SAMPLE "):
            # SAMPLE t=0 ram_mb=.. vram_mb=.. draw_calls=.. chunks=.. fps=..
            kv = dict(_split(kv) for kv in line[len("SAMPLE "):].split() if "=" in kv)
            try:
                samples.append({
                    "t": float(kv.get("t", 0)),
                    "ram_mb": float(kv.get("ram_mb", 0)),
                    "vram_mb": float(kv.get("vram_mb", 0)),
                    "draw_calls": float(kv.get("draw_calls", 0)),
                    "chunks": float(kv.get("chunks", 0)),
                    "fps": float(kv.get("fps", 0)),
                })
            except ValueError:
                pass
        elif "=" in line and " " not in line.split("=", 1)[0]:
            k, _, v = line.partition("=")
            summary[k.strip()] = v.strip()
    if not summary and proc.returncode != 0:
        sys.stderr.write("benchmark 返回非零（%d）；stderr 前几行：\n%s\n"
                         % (proc.returncode, "\n".join(proc.stderr.splitlines()[:10])))
    return samples, summary


def _split(kv):
    k, _, v = kv.partition("=")
    return k.strip(), v.strip()


def stats(values):
    if not values:
        return (0.0, 0.0, 0.0)
    return (min(values), statistics.mean(values), max(values))


def sparkline(values, width=40, lo=None, hi=None):
    """把序列画成 0-9 级别的极简条形。"""
    if not values:
        return ""
    lo = min(values) if lo is None else lo
    hi = max(values) if hi is None else hi
    span = (hi - lo) or 1.0
    chars = " .:-=+*#%@"
    out = []
    step = max(1, len(values) // width)
    for i in range(0, len(values), step):
        v = values[i]
        idx = int((v - lo) / span * (len(chars) - 1))
        idx = max(0, min(len(chars) - 1, idx))
        out.append(chars[idx])
    return "".join(out)


def main():
    args = parse_args()
    samples, summary = run_bench(args.bench, args.quality, args.duration)

    ram = [s["ram_mb"] for s in samples]
    vram = [s["vram_mb"] for s in samples]
    dc = [s["draw_calls"] for s in samples]
    fps = [s["fps"] for s in samples]

    ram_min, ram_avg, ram_max = stats(ram)
    vram_min, vram_avg, vram_max = stats(vram)
    dc_min, dc_avg, dc_max = stats(dc)
    fps_min, fps_avg, fps_max = stats(fps)

    ram_final = float(summary.get("ram_mb", ram_avg))
    vram_final = float(summary.get("vram_mb", vram_avg))
    dc_final = float(summary.get("draw_calls", dc_avg))
    fps_p95 = float(summary.get("fps_p95", fps_avg))
    load_p95 = float(summary.get("load_ms_p95", 0))
    hit = float(summary.get("cache_hit_rate", 0))
    chunks = float(summary.get("chunks_loaded", 0))
    evict = summary.get("lru_evictions", "0")

    lines = []
    a = lines.append
    a("=" * 64)
    a("TASK-036 Low-Spec Profile Report  (quality=%s, duration=%ds)" % (args.quality, args.duration))
    a("=" * 64)
    a("")
    a("阈值门禁（仅 Low 档为硬约束）: ram_mb<=1536  vram_mb<=1024  draw_calls<=300")
    a("")
    a("---- 最终汇总（来自 resource_bench key=value）----")
    a("  ram_mb       = %.2f   (min %.2f / avg %.2f / max %.2f)" % (ram_final, ram_min, ram_avg, ram_max))
    a("  vram_mb      = %.2f   (min %.2f / avg %.2f / max %.2f)" % (vram_final, vram_min, vram_avg, vram_max))
    a("  draw_calls   = %.2f   (min %.2f / avg %.2f / max %.2f)" % (dc_final, dc_min, dc_avg, dc_max))
    a("  fps_p95      = %.2f" % fps_p95)
    a("  load_ms_p95  = %.3f" % load_p95)
    a("  cache_hit    = %.4f" % hit)
    a("  chunks_loaded= %.2f" % chunks)
    a("  lru_evict    = %s" % evict)
    a("")
    a("---- 门禁判定 ----")
    a("  ram_mb   <= 1536 : %s" % ("PASS" if ram_final <= 1536 else "FAIL"))
    a("  vram_mb  <= 1024 : %s" % ("PASS" if vram_final <= 1024 else "FAIL"))
    a("  draw_call<= 300  : %s" % ("PASS" if dc_final <= 300 else "FAIL"))
    a("")
    a("---- RAM 曲线 (MB) ----")
    a("  " + sparkline(ram))
    a("---- VRAM 曲线 (MB) ----")
    a("  " + sparkline(vram))
    a("---- DrawCall 曲线 ----")
    a("  " + sparkline(dc))
    a("---- FPS 曲线 ----")
    a("  " + sparkline(fps))
    a("")
    a("说明：headless 环境下 RAM/VRAM 为真实字节记账（GPU/D3D11 后端为桩），")
    a("      FPS 为按 draw_calls/挂起加载数建模的 CPU 侧帧时间反推值，仅供相对对比。")

    report = "\n".join(lines) + "\n"
    sys.stdout.write(report)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(report)
        sys.stderr.write("报告已写入 %s\n" % os.path.abspath(args.out))


if __name__ == "__main__":
    main()
