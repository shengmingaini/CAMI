"""CAMI 配置加载器：把 data/configs/*.json 解析为 Protobuf 消息。

所有 *ConfigSet 都视为独立热更单元；本模块仅负责"反序列化 + 引用收集"，
真正的平衡校验逻辑在 balance_verifier.py。
"""
import os
import sys
import json
import importlib
import subprocess

# 把生成的桩目录加入搜索路径
_HERE = os.path.dirname(os.path.abspath(__file__))
_PB_DIR = os.path.join(_HERE, "_pb")
if _PB_DIR not in sys.path:
    sys.path.insert(0, _PB_DIR)

from google.protobuf import json_format  # noqa: E402


# 文件名(stem) -> (pb 模块名, ConfigSet 类名)
# 必须在 _ensure_stubs 之前定义：供"生成后完整性校验"使用，避免退化成
# 'ModuleNotFoundError: No module named xxx_pb2' 这种难以定位的幽灵失败。
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


def _ensure_stubs():
    """若生成的 Python 桩缺失、或 .proto 比桩新（防止桩过期），则用当前解释器的
    grpc_tools.protoc 现场重新生成。

    设计约束（吃过的亏）：
    - 桩生成失败必须**显式冒泡**，绝不能静默跳过——否则会退化成
      'ModuleNotFoundError: No module named xxx_pb2' 这种 CI 上极难定位的幽灵失败。
    - 优先用 sys.executable（CI/Linux 与本地 Windows 统一）；禁止硬编码 Windows
      绝对路径，否则 ubuntu runner 上路径不存在会 RuntimeError。
    - 生成后做完整性校验：所有 LOAD_REGISTRY 需要的桩都必须落地，缺失即明确报错。

    注：桩文件（verify/_pb/*.py）已提交仓库，CI 干净检出时哨兵存在 -> need_regen=False，
    直接 import 已验证正确的桩即可，无需运行时 protoc。protoc 仅作为本地 .proto
    变更后重新生成桩的兜底。
    """
    sentinel = os.path.join(_PB_DIR, "config_balance_pb2.py")
    proto_dir = os.path.normpath(os.path.join(_HERE, "..", "proto", "protobuf"))
    proto_files = sorted(
        os.path.join(proto_dir, f) for f in os.listdir(proto_dir) if f.endswith(".proto")
    )
    need_regen = not os.path.exists(sentinel)
    if not need_regen:
        sent_mtime = os.path.getmtime(sentinel)
        if any(os.path.getmtime(pf) > sent_mtime for pf in proto_files):
            need_regen = True
    if not need_regen:
        return

    pyexe = sys.executable
    if not pyexe or not os.path.exists(pyexe):
        raise RuntimeError(
            "找不到可用的 python（需含 grpc_tools.protoc）；请先 `pip install grpcio-tools`"
        )
    os.makedirs(_PB_DIR, exist_ok=True)
    cmd = [pyexe, "-m", "grpc_tools.protoc", "-I", proto_dir,
           "--python_out=" + _PB_DIR] + proto_files
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "protoc 生成桩失败 (exit %d)，请检查 .proto 的 import/语法：\n"
            "--STDOUT--\n%s\n--STDERR--\n%s"
            % (proc.returncode, proc.stdout, proc.stderr)
        )
    # 生成后完整性校验：缺失即显式报错，不退化成 ModuleNotFoundError
    missing = [
        stem for stem in LOAD_REGISTRY
        if not os.path.exists(os.path.join(_PB_DIR, LOAD_REGISTRY[stem][0] + ".py"))
    ]
    if missing:
        raise RuntimeError(
            "protoc 未生成以下桩（疑似对应 .proto 编译被跳过）：%s\n--STDERR--\n%s"
            % (", ".join(missing), proc.stderr)
        )
    print("[load_configs] 已从 .proto 自动生成 Python 桩 ->", _PB_DIR)


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
