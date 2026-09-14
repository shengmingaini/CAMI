#!/usr/bin/env bash
# TASK-036 · Resource / Low Spec System —— 本地验收脚本
#
# 手写刷新（2026-09-14）：Godot 4.7.2 + 2.5D 客户端资源/低配系统。
# 依赖任务包 mmorpg_tasks（任务书位于 mmorpg_tasks/tasks/），用 MMO_TASKS_DIR 显式覆盖。
#
# 环境：Godot 4.7.2（require_godot 门禁）。
# 用法：bash scripts/verify/task-036.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-036" 'Resource / Low Spec System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 034 035

# ---- 2. Godot 4.7.2 门禁（Compatibility 渲染器）----
require_godot

# ---- 3. 交付物存在性（2.5D 资源系统，见任务书 §14 / §23）----
require_files \
  'client/resource/quality_preset.gd' \
  'client/resource/resource_manager.gd' \
  'client/resource/scene_streamer.gd' \
  'client/resource/proc_mesh_cache.gd' \
  'client/config/client/quality.json' \
  'client/resource/tests/test_resource.gd' \
  'client/resource/benchmark/bench_resource.gd' \
  'client/docs/RESOURCE_INTERFACE.md' \
  'client/docs/RESOURCE_PERFORMANCE.md'

# ---- 4. GDScript 解析门禁（--headless --check-only）----
godot_check_only

# ---- 5. headless 单元 + 集成测试 ----
godot_run_tests 'res://resource/tests/test_resource.gd'

# ---- 6. 性能阈值断言：低配档必须守住资源预算（指标缺失直接判失败）--------
if [ -f "$ROOT/bench/resource_low.txt" ]; then
  assert_metric 'bench/resource_low.txt' 'ram_mb'        'le' '512'
  assert_metric 'bench/resource_low.txt' 'draw_calls'    'le' '300'
  grep -q '^verdict_pass=true$' "$ROOT/bench/resource_low.txt" \
    || die "bench/resource_low.txt verdict_pass != true（资源预算超标）"
  ok "bench/resource_low.txt verdict_pass=true"
else
  info "bench/resource_low.txt 尚未生成（benchmark 未跑）；正式验收时必须产出该指标再断言"
fi

# ---- 7. 人工复核项（脚本无法自动判定）----
info "人工复核项（必须人工确认后勾选）："
info "  [ ] 1. 三档画质全部来自 config/client/quality.json，代码无硬编码画质参数"
info "  [ ] 2. 3D 地图分块流式加载：Current + Nearby，越界 5s 延迟卸载"
info "  [ ] 3. 滞回（hysteresis）生效：玩家来回跨越边界不抖动卸载"
info "  [ ] 4. LRU 回收只在 refs==0 时发生，存活引用永不被释放"
info "  [ ] 5. 禁止整个大地图常驻内存（resident chunks 有界）"
info "  [ ] 6. 程序化网格缓存命中复用，无 DCC 模型依赖"
info "  [ ] 7. 低配档保住 4GB RAM / 1GB VRAM 目标（tools/lowspec/profile.gd）"

end_task "TASK-036"
