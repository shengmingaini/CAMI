# task — 任务执行留档

每个 TASK 的验收输出（构建日志、Benchmark 数字、Failure Test 记录）归档于本目录，按 `TASK-NNN/` 子目录组织。

- 任务权威定义：`../mmorpg_tasks/tasks/TASK-NNN.md`
- 验收脚本：`../scripts/verify/task-NNN.sh`
- 完成流程：`scripts/task-done.sh TASK-NNN` → 校验 STATUS 流转 → 提交

TASK-000 仅建立工程骨架，不含任何游戏功能代码；本目录初始为占位。
