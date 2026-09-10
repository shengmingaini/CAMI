#!/usr/bin/env bash
# TASK-005 · Protocol Schema（Protobuf + FlatBuffers） —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-005.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-005.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-005" 'Protocol Schema（Protobuf + FlatBuffers）'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 001

# ---- 2. 交付物存在性 ----
require_files \
  'protocol/proto/envelope.proto' \
  'protocol/flatbuffers/movement.fbs' \
  'protocol/docs/VERSIONING.md' \
  'protocol/proto/common.proto' \
  'protocol/proto/command.proto' \
  'protocol/proto/query.proto' \
  'protocol/proto/event.proto' \
  'protocol/flatbuffers/aoi.fbs' \
  'protocol/flatbuffers/combat.fbs' \
  'protocol/docs/INTERFACE.md' \
  'protocol/CMakeLists.txt'

# ---- 3. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 4. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Protocol' 'Protocol'

# ---- 5. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/protocol_bench' --iterations 1000000
assert_metric 'bench/protocol.txt' 'fbs_decode_ns' 'le' '300'
assert_metric 'bench/protocol.txt' 'fbs_decode_allocs' 'le' '0'

# ---- 6. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. MessageEnvelope 九项字段全部定义且 C++ 可 Encode/Decode（单测断言每个字段往返一致）"
info "  [ ] 2. 版本不匹配时返回 `VERSION_CONFLICT`，**不静默降级**"
info "  [ ] 3. FlatBuffers 解码路径分配次数 = 0（零拷贝，benchmark 实测）"
info "  [ ] 4. 1000 次随机截断 fuzz 无崩溃、无越界（ASan 下跑）"
info "  [ ] 5. 生成代码不入库（`git status` 不出现 *.pb.h / *_generated.h）"
info "  [ ] 6. protocol/docs/VERSIONING.md 存在且写明演进规则"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Protocol 全绿"

end_task "TASK-005"
