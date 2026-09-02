# AiSystem · TEST

## 单元测试（ctest -R Ai，§16）

`ai_test.cpp` 覆盖：

1. **六态全转移路径**：Idle→Patrol→Chase→Attack→Return→Idle（含目标进入/贴近/远离/回家）。
2. **决策节流**：1000 怪 1 秒决策次数落在 5Hz 带宽（4000~7000），远低于每 Tick 全量的 ~20000。
3. **目标选取**：AOI 局部选最近敌对；目标下线后切换/清空，不卡死在 Chase。
4. **死亡 + 重生**：致命伤害转 Dead；Scheduler 越过重生延迟后 Revive 触发，状态/位置复位。
5. **生命周期**：Spawn/Despawn 计数与状态分布一致；销毁未知实体返回 NOT_FOUND。
6. **配置化**：`LoadSpawnDefsFromDir` 从 JSON 取值（max_hp/aggro 等），缺失文件返回错误。

## 集成 / 长稳（§17 / §18）

`ai_bench`（1000 怪 + 50 玩家，12000 tick ≈ 10 分钟）验证：无死锁、无泄漏、状态分布可观测、
AI 阶段耗时达标。bench 输出 `bench/ai.txt` 供验收脚本断言 `ai_phase_us_at_1k ≤ 400` / `mem_bytes_per_ai ≤ 128`。

## 失败用例（§19）

- 目标突然消失 → Return 而非卡死 Chase（见 test_target_selection）。
- 1000 怪被同时拉仇恨 → 走 AOI 局部查询，无 1000 次全量扫描（benchmark + grep 佐证）。
- 配置缺失字段 → 加载失败返回错误，禁止默认值静默生成（见 test_config_loader）。
