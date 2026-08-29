#!/usr/bin/env bash
# TASK-034 · Client Core —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-034.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-034.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-034" 'Client Core'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 005

# ---- 2. 交付物存在性 ----
require_files \
  'client/core/include/mmo/client/net_client.h' \
  'config/client/client.json'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/client/core/include" ]; then
  scan_forbidden 'client/core/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Client' 'Client'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/client_bench' --frames 600
assert_metric 'bench/client.txt' 'frame_ms_p95' 'le' '16.6'
assert_metric 'bench/client.txt' 'mem_bytes_client_base' 'le' '157286400'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **客户端协议直接复用 TASK-005**（grep：client 目录无独立的消息头定义）"
info "  [ ] 2. **客户端不依赖任何服务端模块**（链接检查：client 目标不 link server 库）"
info "  [ ] 3. 帧循环 60Hz 精度误差 < 100ms/10 秒，CatchUp 限幅生效"
info "  [ ] 4. 连接 → 登录 → 收快照 → 插值全链路跑通（集成测试）"
info "  [ ] 5. 断网重连可用，画面冻结而非崩溃"
info "  [ ] 6. 损坏快照被丢弃且不崩溃"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Client 全绿"

end_task "TASK-034"
