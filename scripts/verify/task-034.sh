#!/usr/bin/env bash
# TASK-034 · Client Core —— 本地验收脚本
#
# 手写刷新（2026-09-14）：按 docs/client-spec-2.5d.md §8/§12 从「自研 C++ 客户端」
# 切换到 Godot 4.7.2 + 2.5D。
# ⚠ 禁止用 tools/gen/build_tasks.py 重新生成：生成器会把全部 42 份任务书 STATUS
#    重置为 PENDING，清空已完成台账（见技能 §2.2）。
#
# 环境：Godot 4.7.2（require_godot 门禁）；GDExtension(C++) 需 MinGW/MSYS2 或 MSVC。
# 用法：bash scripts/verify/task-034.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-034.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-034" 'Client Core'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 005

# ---- 2. Godot 4.7.2 门禁（Compatibility 渲染器：只需 OpenGL 3.3 / D3D11）----
require_godot

# ---- 3. 交付物存在性（2.5D Godot 结构，见任务书 §14 / §23）----
require_files \
  'client/project.godot' \
  'client/runtime/autoload/game_loop.gd' \
  'client/runtime/autoload/client_world.gd' \
  'client/runtime/autoload/input_manager.gd' \
  'client/runtime/autoload/config.gd' \
  'client/network/net_client.gd' \
  'client/extensions/protocol_codec/src/protocol_codec.cpp'

# ---- 4. GDScript 解析门禁（--headless --check-only）----
godot_check_only

# ---- 5. headless 单元测试 ----
godot_run_tests 'res://runtime/tests/test_client_core.gd'

# ---- 6. 性能阈值断言：帧必须稳住 60Hz（P95 < 16.6ms）------------------------
# 指标缺失直接判失败，禁止用估算值代替；benchmark 尚未产出时给明确提示。
if [ -f "$ROOT/bench/client.txt" ]; then
  assert_metric 'bench/client.txt' 'frame_ms_p95' 'le' '16.6'
  assert_metric 'bench/client.txt' 'mem_bytes_client_base' 'le' '157286400'
else
  info "bench/client.txt 尚未生成（benchmark 未跑）；正式验收时必须产出该指标再断言"
fi

# ---- 7. 人工复核项（脚本无法自动判定）----
info "人工复核项（必须人工确认后勾选）："
info "  [ ] 1. 客户端协议直接复用 TASK-005（GDExtension 绑定，GDScript 无独立协议实现）"
info "  [ ] 2. 客户端不依赖任何服务端模块（只通过 Protocol 契约 + protocol_codec 绑定）"
info "  [ ] 3. 帧循环 60Hz 精度误差 < 100ms/10 秒，CatchUp 限幅生效"
info "  [ ] 4. 连接 → 登录 → 收快照 → 插值全链路跑通（集成测试，需 GDExtension 已构建）"
info "  [ ] 5. 断网重连可用，画面冻结而非崩溃"
info "  [ ] 6. 损坏快照被丢弃且不崩溃"
info "  [ ] 7. GDExtension（protocol_codec）双构建通过"

end_task "TASK-034"
