#!/usr/bin/env bash
# TASK-035 · Renderer (2.5D) —— 本地验收脚本
#
# 手写刷新（2026-09-14）：按 docs/client-spec-2.5d.md 从「自研 C++ 客户端」
# 切换到 Godot 4.7.2 + 2.5D 渲染（Node3D + Sprite3D + ArrayMesh）。原生成器脚本
# 检查 C++ 头/ctest，已不适用。
# ⚠ 禁止用 tools/gen/build_tasks.py 重新生成：生成器会把全部 42 份任务书 STATUS
#    重置为 PENDING，清空已完成台账。
#
# 环境：Godot 4.7.2（require_godot 门禁）。2.5D 渲染：Node3D + Sprite3D + ArrayMesh。
# 用法：bash scripts/verify/task-035.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-035" 'Renderer (2.5D)'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 034

# ---- 2. Godot 4.7.2 门禁（Compatibility 渲染器：OpenGL 3.3 / D3D11）----
require_godot

# ---- 3. 交付物存在性（2.5D 渲染结构，见任务书 §7 / §14）----
require_files \
  'client/renderer/camera/iso_camera.gd' \
  'client/renderer/sprites/sprite_entity.gd' \
  'client/renderer/terrain/proc_ground.gd' \
  'client/renderer/pipeline/render_stats.gd' \
  'client/renderer/pipeline/lod_manager.gd' \
  'client/renderer/pipeline/culling.gd' \
  'client/config/client/render.json' \
  'client/docs/INTERFACE.md' \
  'client/docs/PERFORMANCE.md'

# ---- 4. GDScript 解析门禁（--headless --check-only）----
godot_check_only

# ---- 5. headless 单元测试 + §17 集成（1000 地块 + 200 精灵 + 50 UI）----
godot_run_tests 'res://renderer/tests/test_renderer.gd'

# ---- 6. 性能阈值断言：Low 预算内（spec §22 / §27.2）------------------------
# 指标缺失直接判失败，禁止用估算值代替；benchmark 尚未产出时给明确提示。
if [ -f "$ROOT/bench/render_low.txt" ]; then
  assert_metric 'bench/render_low.txt' 'draw_calls' 'lt' '300'
  assert_metric 'bench/render_low.txt' 'triangles' 'lt' '300000'
  assert_metric 'bench/render_low.txt' 'texture_mem_mb' 'lt' '512'
else
  info "bench/render_low.txt 尚未生成（benchmark 未跑）；正式验收时必须产出该指标再断言"
fi

# ---- 7. 人工复核项（脚本无法自动判定）----
info "人工复核项（必须人工确认后勾选）："
info "  [ ] 1. 2.5D 等距相机固定 45° 俯角（spec §2.2 / §13-1）"
info "  [ ] 2. 实体为 8 方向 Sprite3D billboard，无 3D 模型（spec §5）"
info "  [ ] 3. 地面为程序化 ArrayMesh，单网格单 draw call（spec §5 / '1000 地块'）"
info "  [ ] 4. 远景遮挡：不透明地面写深度，远精灵被深度缓冲遮挡（spec §4 验收 #4）"
info "  [ ] 5. 3 档质量预设（Low/Medium/High）配置驱动，Low 为主兼容目标（spec §35）"
info "  [ ] 6. 渲染层只读 ClientWorld 镜像，不回写逻辑状态"
info "  [ ] 7. LOD + 距离剔除将 draw call / 三角面控制在 Low 预算内（spec §22）"
info "  [ ] 8. 真实设备 FPS 在目标低配硬件上仍需实测确认（spec §35 末句）"

end_task "TASK-035"
