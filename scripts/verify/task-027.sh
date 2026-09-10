#!/usr/bin/env bash
# TASK-027 · Redis Adapter —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-027.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-027.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-027" 'Redis Adapter'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 026

# ---- 2. 交付物存在性 ----
require_files \
  'server/dataservice/include/mmo/data/redis/redis_cache.h' \
  'docker/redis/docker-compose.yml'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'server/dataservice/src/redis' '\"KEYS\"'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/dataservice/include" ]; then
  scan_forbidden 'server/dataservice/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 端口占用检查（真实实例须在线监听 6379，§20.1）----
require_port_open 6379

# ---- 6. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 7. 单元测试（ctest 过滤执行；注册名为 DataService.Redis，§16/§17）----
run_ctest "$BUILD_TYPE" 'DataService.Redis' 'DataService.Redis'

# ---- 8. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/redis_bench' --ops 10000
assert_metric 'bench/redis.txt' 'get_ns' 'le' '200000'
assert_metric 'bench/redis.txt' 'pool_acquire_ns' 'le' '1000'

# ---- 9. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **连接真实 Redis 的集成测试全部通过**（本地 docker 或 Windows 版 Redis）"
info "  [ ] 2. ICache 四个方法全部实现，行为与内存实现一致（同一套接口测试）"
info "  [ ] 3. Session / Cache / Routing 三类用途均可用"
info "  [ ] 4. **禁止使用 KEYS 命令**（MONITOR 验证 + grep 代码）"
info "  [ ] 5. 密码从环境变量读取，代码中无明文口令（grep 验证）"
info "  [ ] 6. 连接池耗尽/Redis 宕机/慢查询三种故障行为明确且有测试"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R DataService 全绿（无 Redis 时 `[redis]` 用例应明确 skip 而非伪装通过）"

end_task "TASK-027"
