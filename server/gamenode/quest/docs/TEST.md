# QuestSystem · TEST

## 单元测试（ctest -R Quest，§16）

`quest_test.cpp`（14 个测试函数，186 项 CHECK，全部走 `mmo::core::test` 输出）覆盖：

1. **配置加载**：`LoadQuests` 从目录读取 5 条任务；缺字段/未知类型/数量 0/目录不存在
   一律返回明确错误码（禁止默认值静默生成）。
2. **接取校验**：等级门槛（经 `IPlayerQuery`）、前置任务链（须 `TurnedIn` 才算满足）、
   重复接取 → `BUSY`、未知任务 → `NOT_FOUND`。
3. **四类事件驱动进度**：KillMonster（1001×3）、CollectItem（9001×5）、TalkNpc（2002×1）、
   ReachLocation（3001×1），进度按事件增量推进，达标转 `Completed`。
4. **多目标完成**：1005 含 KillMonster(1002×8)+CollectItem(9003×2)，逐个目标达标后整体完成。
5. **交还幂等**：已交还的任务重复 `TurnIn` 返回 Ok 但**不重复发奖**（`RewardGrantCount` 不变）。
6. **放弃**：清空进度 + 退出索引，放弃后事件不再命中，可重接且进度重置。
7. **事件总线绑定（集成）**：`BindEventBus` 订阅四类事件 → 转 `OnEvent`；
   `PublishImmediate` 同线程立即派发、`Publish`+`Drain` 异步派发，进度语义与直接 `OnEvent` 一致；
   `BindEventBus` **幂等**（重复调用不翻倍、不重复派发单次事件）。
8. **反模式验证（§19）**：`test_no_player_scan` 实测 1k→10k 玩家耗时比 ≈ 1.5，证明单次事件处理
   与玩家总数无关（无「遍历所有玩家」的反模式）。
9. **发奖失败可重试（§19）**：`IRewardSink::Grant` 返回 `BUSY` 时 `TurnIn` 返回失败、保持
   `Completed`、不标记 `TurnedIn`；重试后成功发奖。
10. **1000 条配置加载耗时** < 50ms；`QuestInstance` 结构 ≤ 128B。

## 集成 / 长稳（§17 / §18）

`quest_bench`（--players 1000 --events 10000）输出 `bench/quest.txt`，验收脚本断言：
`event_handle_ns ≤ 1000`、`scaling_check_1k_vs_10k_players ≤ 1.5`。

## 失败用例（§19）

- 未交还前置任务即接取 → `INVALID_ARGUMENT`（见 `test_prerequisite_chain`）。
- 发奖失败 → 不置终态、可重试，重试后成功（见 `test_reward_failure_is_retryable`）。
- 10k 玩家规模下单事件处理耗时与 1k 玩家同量级（见 `test_no_player_scan`），不存在玩家全扫描。
- 配置缺失字段 → 加载失败返回错误，禁止默认值静默生成（见 `test_config_errors`）。
