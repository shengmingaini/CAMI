#!/usr/bin/env bash
# TASK-028 · MySQL Adapter —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-028.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-028.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-028" 'MySQL Adapter'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 026

# ---- 2. 交付物存在性 ----
require_files \
  'database/migrations/001_init.sql' \
  'server/dataservice/include/mmo/data/mysql/shard_router.h' \
  'server/dataservice/include/mmo/data/mysql/mysql_store.h' \
  'server/dataservice/include/mmo/data/mysql/connection_pool.h' \
  'docker/mysql/docker-compose.yml' \
  'server/dataservice/docs/SCHEMA.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/dataservice/include" ]; then
  scan_forbidden 'server/dataservice/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 端口在线检查（真实外部实例：端口须已在线监听，§20.1）----
#    语义与 require_free_port 相反：真实实例类任务要求端口被实例占用而非空闲。
require_port_open 3306

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'DataService.MySql' 'DataService.MySql'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/mysql_bench' --ops 10000
assert_metric 'bench/mysql.txt' 'select_ns' 'le' '1000000'
assert_metric 'bench/mysql.txt' 'pool_acquire_ns' 'le' '5000'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **连真实 MySQL 的集成测试全部通过**"
info "  [ ] 2. 7 张逻辑表（Account/Character/Inventory/Equipment/Quest/Guild/Mail）建表脚本齐全"
info "  [ ] 3. **分片数未被写死到业务层**（grep：业务代码无 shard_count / % 8）"
info "  [ ] 4. 乐观锁（version 列）生效，冲突返回 VERSION_CONFLICT"
info "  [ ] 5. 事务与回滚正确，跨分片事务被明确拒绝"
info "  [ ] 6. 迁移工具可用（up / status / dry-run）"
info "  [ ] 7. 密码只存 salted hash（grep 无明文密码存储）"
info "  [ ] 8. Debug / Release 双构建通过，无 MySQL 时 `[mysql]` 用例明确 skip 而非伪装通过"

end_task "TASK-028"
