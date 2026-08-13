# CAMI 任务/进度/奖励 — Cap'n Proto 统一协议 (阶段 A)
# 转写自 proto/flatbuffers/quest.fbs。轨道 B 可靠消息。
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xcA11c0de00000006;

using Common = import "common.capnp";

enum QuestStatus :UInt8 {
  accepted;     # 进行中
  complete;     # 目标已达成, 待交付
  turnedIn;     # 已交付领奖
}

enum QuestTurnInResult :UInt8 {
  ok;
  notComplete;     # 目标未达成
  prereqFail;      # 前置/等级不满足
  inventoryFull;   # 背包已满
}

# 单任务运行态 (首包全量 + 增量引用)
struct QuestLogEntry {
  questId @0 :UInt32;
  status @1 :QuestStatus;
  objectiveProgress @2 :List(UInt32);   # 与 config_quests.objectives 对齐
}

# ---- C->S 意图 ----
struct QuestAcceptIntent {
  playerId @0 :UInt64;
  questId @1 :UInt32;
  npcKey @2 :Text;       # 接取 NPC
}

struct QuestTurnInIntent {
  playerId @0 :UInt64;
  questId @1 :UInt32;
  npcKey @2 :Text;       # 交付 NPC
  chosenRewardIndex @3 :UInt32;  # 多选一奖励下标, 无则 0
}

# ---- S->C 结果/增量 ----
struct QuestProgress {
  playerId @0 :UInt64;
  questId @1 :UInt32;
  objectiveIdx @2 :UInt32;
  progress @3 :UInt32;   # 当前计数 (增量下发, 非全量重发)
}

struct QuestRewardResult {
  playerId @0 :UInt64;
  questId @1 :UInt32;
  result @2 :QuestTurnInResult;
  exp @3 :UInt64;
  money @4 :UInt64;
  items @5 :List(Common.ItemGrant);
}

struct QuestLogSnapshot {
  playerId @0 :UInt64;
  entries @1 :List(QuestLogEntry);   # 首包全量; 后续仅 QuestProgress 差分
}
