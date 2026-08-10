"""CAMI 配置加载器：把 data/configs/*.json 解析为 Protobuf 消息。

所有 *ConfigSet 都视为独立热更单元；本模块仅负责"反序列化 + 引用收集"，
真正的平衡校验逻辑在 balance_verifier.py。
"""
import os
import sys
import json
import importlib

# 把生成的桩目录加入搜索路径
_HERE = os.path.dirname(os.path.abspath(__file__))
_PB_DIR = os.path.join(_HERE, "_pb")
if _PB_DIR not in sys.path:
    sys.path.insert(0, _PB_DIR)

from google.protobuf import json_format  # noqa: E402

import subprocess  # noqa: E402


def _ensure_stubs():
    """若生成的 Python 桩缺失、或 .proto 比桩新（防止桩过期），则用受管 venv 的
    protoc 现场重新生成。让校验工程在干净检出 / proto 变更后可一键运行。"""
    sentinel = os.path.join(_PB_DIR, "config_balance_pb2.py")
    proto_dir = os.path.normpath(os.path.join(_HERE, "..", "proto", "protobuf"))
    proto_files = [os.path.join(proto_dir, f) for f in os.listdir(proto_dir) if f.endswith(".proto")]
    need_regen = not os.path.exists(sentinel)
    if not need_regen:
        sent_mtime = os.path.getmtime(sentinel)
        if any(os.path.getmtime(pf) > sent_mtime for pf in proto_files):
            need_regen = True
    if not need_regen:
        return
    # 优先用当前解释器（CI / 跨平台通用：当前 python 已含 grpc_tools.protoc）；
    # 仅在本机 Windows 回退到受管 venv 的 python.exe（隔离环境）。
    # 注意：不能硬编码 Windows 绝对路径——GitHub ubuntu runner 上该路径不存在，
    # 会导致 economy-balance job 现场生成桩时 RuntimeError 而 CI 必红。
    candidates = [sys.executable]
    if sys.platform.startswith("win"):
        candidates += [
            r"C:/Users/17283/.workbuddy/binaries/python/envs/default/Scripts/python.exe",
            os.path.join(os.environ.get("HOME", ""), ".workbuddy", "binaries", "python", "envs", "default", "Scripts", "python.exe"),
        ]
    pyexe = next((c for c in candidates if c and os.path.exists(c)), None)
    if pyexe is None:
        raise RuntimeError("找不到可用的 python（需含 grpc_tools.protoc）；请先 pip install grpcio-tools 或运行 protoc 生成 verify/_pb")
    os.makedirs(_PB_DIR, exist_ok=True)
    cmd = [pyexe, "-m", "grpc_tools.protoc", "-I", proto_dir,
           "--python_out=" + _PB_DIR] + [
        os.path.join(proto_dir, f) for f in os.listdir(proto_dir) if f.endswith(".proto")]
    subprocess.run(cmd, check=True)
    print("[load_configs] 已从 .proto 自动生成 Python 桩 ->", _PB_DIR)

# 文件名(stem) -> (pb 模块名, ConfigSet 类名)
LOAD_REGISTRY = {
    "currencies":   ("config_currencies_pb2", "CurrencyConfigSet"),
    "balance":      ("config_balance_pb2",    "BalanceConfigSet"),
    "items":        ("config_items_pb2",       "ItemConfigSet"),
    "loot":         ("config_loot_pb2",        "LootTableSet"),
    "creatures":    ("config_creatures_pb2",   "CreatureConfigSet"),
    "spawns":       ("config_spawns_pb2",      "SpawnConfigSet"),
    "vendors":      ("config_vendors_pb2",     "VendorConfigSet"),
    "pvp":          ("config_pvp_pb2",          "PvpConfigSet"),
    "world_events": ("config_world_events_pb2","WorldEventConfigSet"),
    "reputation":   ("config_reputation_pb2",  "FactionConfigSet"),
    "professions":  ("config_professions_pb2",  "ProfessionConfigSet"),
    "zones":        ("config_zones_pb2",        "ZoneConfigSet"),
    "stats":        ("config_stats_pb2",        "StatConfigSet"),
}


def _load_one(path):
    mod_name, cls_name = LOAD_REGISTRY[os.path.splitext(os.path.basename(path))[0]]
    mod = importlib.import_module(mod_name)
    cls = getattr(mod, cls_name)
    msg = cls()
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    # 默认 strict：键必须匹配 proto 字段名（snake_case），便于暴露数据笔误
    json_format.Parse(text, msg, ignore_unknown_fields=False)
    return msg


def load_all(data_dir):
    """加载目录下所有已知 ConfigSet，返回 {stem: message}。"""
    _ensure_stubs()
    result = {}
    for stem in LOAD_REGISTRY:
        path = os.path.join(data_dir, stem + ".json")
        if os.path.exists(path):
            result[stem] = _load_one(path)
    return result


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    dd = os.path.join(os.path.dirname(here), "data", "configs")
    cfgs = load_all(dd)
    for k, v in cfgs.items():
        # 找到第一个 repeated 字段名（即条目列表），报告条数
        rep_name = next(f.name for f in v.DESCRIPTOR.fields if f.label == f.LABEL_REPEATED)
        print(f"{k:14s} loaded OK  ({len(getattr(v, rep_name))} entries)")
    print("TOTAL", len(cfgs), "config sets")
