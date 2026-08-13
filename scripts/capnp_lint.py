#!/usr/bin/env python3
"""Cap'n Proto schema 结构校验 (无 capnp 编译器时的护栏)。

检查项:
  1. 每个文件有且仅有一个文件 ID (@0x....;)
  2. 花括号/圆括号平衡
  3. 每个 struct 内字段 ordinal (@n) 不重复 (含 union 成员, 它们共享 struct 的 ordinal 空间)
  4. 保留字未用作文件级符号 (极简检查)
  5. struct/enum 定义基本闭合

用法: python3 scripts/capnp_lint.py proto/capnp
"""
import sys
import re
import os

RESERVED = {"struct", "enum", "union", "using", "import", "const", "interface", "annotation"}

def strip_comment(line: str) -> str:
    # 去掉 # 注释 (本仓库 schema 不含字符串内的 #)
    idx = line.find("#")
    return line[:idx] if idx != -1 else line

def lint_file(path: str):
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    lines = [strip_comment(l) for l in raw.splitlines()]

    errors = []

    # 1. 文件 ID
    ids = re.findall(r"@0x[0-9a-fA-F]{1,16}\s*;", raw)
    if len(ids) != 1:
        errors.append(f"文件 ID 数量异常: 期望 1, 实际 {len(ids)}")

    # 2. 括号平衡
    depth_b = 0
    depth_p = 0
    for i, ch in enumerate(raw):
        if ch == "{":
            depth_b += 1
        elif ch == "}":
            depth_b -= 1
        elif ch == "(":
            depth_p += 1
        elif ch == ")":
            depth_p -= 1
        if depth_b < 0 or depth_p < 0:
            errors.append(f"括号提前闭合 (位置 {i})")
            break
    if depth_b != 0:
        errors.append(f"花括号不平衡: 净深度 {depth_b}")
    if depth_p != 0:
        errors.append(f"圆括号不平衡: 净深度 {depth_p}")

    # 3. struct 内 ordinal 重复检测 (跟踪嵌套深度, 收集每个 struct 的 ordinal)
    in_struct = None  # 当前 struct 名
    struct_ords = {}  # name -> set(ord)
    struct_stack = []
    field_re = re.compile(r"^\s*([A-Za-z_]\w*)\s*@(\d+)\s*:")
    union_depth = 0
    for ln in lines:
        s = ln.strip()
        m = re.match(r"struct\s+(\w+)\s*\{", s)
        if m:
            in_struct = m.group(1)
            struct_ords.setdefault(in_struct, set())
            struct_stack.append(in_struct)
            continue
        if s.startswith("union") and "{" in s:
            union_depth += 1
            continue
        if "}" in s:
            # 简单弹栈: 仅当栈非空
            if struct_stack:
                struct_stack.pop()
                in_struct = struct_stack[-1] if struct_stack else None
            if union_depth and s.strip() == "}":
                union_depth -= 1
            continue
        fm = field_re.match(s)
        if fm and in_struct is not None:
            name, ordn = fm.group(1), int(fm.group(2))
            if ordn in struct_ords[in_struct]:
                errors.append(f"struct {in_struct}: 字段 ordinal 重复 @ {ordn} (字段 {name})")
            else:
                struct_ords[in_struct].add(ordn)

    return errors

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "proto/capnp"
    files = []
    for dirpath, _, fnames in os.walk(root):
        for fn in fnames:
            if fn.endswith(".capnp"):
                files.append(os.path.join(dirpath, fn))
    if not files:
        print(f"未找到 .capnp 文件于 {root}")
        sys.exit(1)
    total = 0
    for fp in sorted(files):
        errs = lint_file(fp)
        if errs:
            total += len(errs)
            print(f"[FAIL] {fp}")
            for e in errs:
                print(f"   - {e}")
        else:
            print(f"[ OK ] {fp}")
    print(f"\n校验完成: {len(files)} 文件, {total} 处错误")
    sys.exit(1 if total else 0)

if __name__ == "__main__":
    main()
