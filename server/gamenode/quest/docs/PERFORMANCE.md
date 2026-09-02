# QuestSystem · PERFORMANCE

性能预算（§22 / 验收脚本）：

| 指标 | 含义 | 阈值 |
|---|---|---|
| `event_handle_ns` | 单次 `OnEvent` 平均耗时（ns） | ≤ 1000 |
| `scaling_check_1k_vs_10k_players` | 1k vs 10k 玩家同批事件的耗时比 | ≤ 1.5 |
| `config_load_ms_for_1000` | 1000 条任务配置加载耗时（ms） | < 50 |
| `mem_bytes_per_quest` | 单任务实例净堆占用（字节） | 观察项 |
| `quest_update_per_1k_events_us` | 1000 条事件累计更新耗时（μs） | 观察项 |

## 关键设计（保证阈值达标）

1. **倒排索引 O(1) 定位**：键 `(ObjectiveType, target_id, PlayerId)` 三元组 → 桶内
   `vector<Entry>`。`OnEvent` 只改命中条目，耗时与玩家总数、任务总数**无关**（§20.2 反模式红线）。
2. **无每 Tick 全量扫描**：不遍历所有玩家检查所有任务；进度只由事件触发。反模式测试
   `test_no_player_scan` 注册高目标数任务（进度永不达标 → 命中集恒定），实测 1k→10k 玩家
   耗时比 ≈ 1.5，证明不存在「遍历所有玩家」的反模式。
3. **目标达标即退出索引**：`cur >= required_count` 时 `EraseEntry`，后续同类事件零命中直接返回。
4. **幂等交还**：`player+quest` 去重表（`turned_in_`），重复 `TurnIn` 不重发奖励、不重复计发奖。

## 实测（以验收脚本 `bench/quest.txt` 输出为准，示例）

```
players=1000 events=10000 event_handle_ns=26.300
quest_update_per_1k_events_us=26.300 mem_bytes_per_quest=316.602
scaling_check_1k_vs_10k_players=1.469 config_load_ms_for_1000=10.451
index_bucket_count=1024 index_entry_count=1000
```

- `event_handle_ns=26.3`（≤ 1000 ✅）
- `scaling_check_1k_vs_10k_players=1.469`（≤ 1.5 ✅）
- `config_load_ms_for_1000=10.451`（< 50 ✅）
- `QuestInstance` 结构体 = 48B（≤ 128 预算，单测 `test_instance_footprint` 断言）

> 注：`mem_bytes_per_quest` 含 `QuestInstance` + 进度 `vector` + 索引条目 + `unordered_map`
> 节点等净堆增量；运行期放大到 20k 任务时约 316B/任务，远低于宽松预算。
