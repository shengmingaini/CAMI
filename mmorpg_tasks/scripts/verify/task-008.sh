#!/usr/bin/env bash
# TASK-008 · Network Transport（TCP 第一版） —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-008.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-008.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-008" 'Network Transport（TCP 第一版）'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 004 005

# ---- 2. 交付物存在性 ----
require_files \
  'engine/net/include/mmo/net/transport.h' \
  'engine/net/docs/PERFORMANCE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/engine/net/include" ]; then
  scan_forbidden 'engine/net/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Net' 'Net'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/net_bench' --connections 10000 --duration 60
assert_metric 'bench/net_10k.txt' 'per_conn_mem_kb' 'le' '20'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 1K / 5K / 10K 连接基准全部跑完，结果数据写入 docs/PERFORMANCE.md（含 CPU / 内存 / PPS）"
info "  [ ] 2. 半包与粘包处理正确（单测 + 集成测试双重覆盖）"
info "  [ ] 3. 单连接发送缓冲背压生效，不出现无界内存增长（长稳 10 分钟 RSS 平稳）"
info "  [ ] 4. 异常断连可被感知并回收资源，连接计数准确回落"
info "  [ ] 5. 传输层**不含任何游戏逻辑**（grep：net/ 目录无 combat/scene/quest 字样）"
info "  [ ] 6. 上层只依赖 INetworkTransport 抽象（grep：除 factory 外无 TcpTransport 直接引用）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Net 全绿"

end_task "TASK-008"
