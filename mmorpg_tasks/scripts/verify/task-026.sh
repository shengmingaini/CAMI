#!/usr/bin/env bash
# TASK-026 · DataService Interface —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-026.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-026.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-026" 'DataService Interface'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 005 006

# ---- 2. 交付物存在性 ----
require_files \
  'server/dataservice/include/mmo/data/data_service.h' \
  'protocol/proto/service/data_service.proto' \
  'server/dataservice/include/mmo/data/idata_store.h' \
  'server/dataservice/include/mmo/data/icache.h' \
  'server/dataservice/include/mmo/data/irepository.h' \
  'server/dataservice/docs/INTERFACE.md' \
  'server/dataservice/docs/README.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/dataservice/include" ]; then
  scan_forbidden 'server/dataservice/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'DataService' 'DataService'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/data_bench' --ops 100000
assert_metric 'bench/data.txt' 'load_ns' 'le' '500'
assert_metric 'bench/data.txt' 'cache_hit_ns' 'le' '300'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. IRepository / IDataStore / ICache 三套接口定义完成，无具体数据库依赖"
info "  [ ] 2. Load / Save / Update / Delete / Batch / VersionCheck 六种操作语义齐全"
info "  [ ] 3. **版本冲突返回 VERSION_CONFLICT，不静默覆盖**（单测断言）"
info "  [ ] 4. 内存实现可跑通全部测试（无需真实数据库即可验收）"
info "  [ ] 5. 批量操作失败时逐条返回结果（禁整批吞错）"
info "  [ ] 6. data_service.proto 定义完成，RPC 签名与接口一致"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R DataService 全绿"

end_task "TASK-026"
