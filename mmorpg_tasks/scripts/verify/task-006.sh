#!/usr/bin/env bash
# TASK-006 · RPC Framework（gRPC 统一封装） —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-006.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-006.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-006" 'RPC Framework（gRPC 统一封装）'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 001 005

# ---- 2. 交付物存在性 ----
require_files \
  'engine/rpc/include/mmo/rpc/status_mapping.h' \
  'engine/rpc/docs/INTERFACE.md' \
  'engine/rpc/include/mmo/rpc/grpc_client.h' \
  'engine/rpc/include/mmo/rpc/grpc_server.h' \
  'engine/rpc/include/mmo/rpc/grpc_channel_pool.h' \
  'engine/rpc/docs/PERFORMANCE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/engine/rpc/include" ]; then
  scan_forbidden 'engine/rpc/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Rpc' 'Rpc'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/rpc_bench' --iterations 100000
assert_metric 'bench/rpc.txt' 'rpc_p99_us' 'le' '1000'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. gRPC status → Error 映射表 100% 覆盖（表驱动单测）且全仓唯一实现"
info "  [ ] 2. 超时、取消、重试成功、重试耗尽四种路径均有集成测试且通过"
info "  [ ] 3. **非幂等调用重试次数 = 0**（单测断言服务端只收到 1 次）"
info "  [ ] 4. TraceID 跨进程透传验证通过（服务端日志含客户端 trace_id）"
info "  [ ] 5. 优雅关闭：在途请求完成，grace 超时后强制关闭且记录日志"
info "  [ ] 6. grep 验证：engine/rpc 之外无 `grpc::` 直接调用（统一走封装）"
info "  [ ] 7. benchmark 输出 p50/p99/QPS 并写入 docs/PERFORMANCE.md"
info "  [ ] 8. Debug / Release 双构建通过，ctest -R Rpc 全绿"

end_task "TASK-006"
