# CAMI 好友/公会/组队/聊天/声望/成就 — Cap'n Proto 统一协议 (阶段 A)
# 转写自 proto/flatbuffers/social.fbs。轨道 B 可靠消息。
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xcA11c0de00000008;

enum ChatChannel :UInt8 {
  whisper;   # 私聊 (指定 toId)
  party;     # 队伍
  guild;     # 公会
  world;     # 世界
  system;    # 系统
}

# ---- C->S 意图 ----
struct FriendAdd {
  playerId @0 :UInt64;
  targetId @1 :UInt64;
}

struct GuildCreate {
  leaderId @0 :UInt64;
  name @1 :Text;
}

struct PartyInvite {
  leaderId @0 :UInt64;
  targetId @1 :UInt64;
}

struct ChatSend {
  fromId @0 :UInt64;
  channel @1 :ChatChannel;
  toId @2 :UInt64;     # Whisper 时为目标, 其余为 0
  payload @3 :Text;    # 已脱敏 (anti_cheat 预处理)
}

# ---- S->C 结果/广播 ----
struct FriendEntry {
  friendId @0 :UInt64;
  online @1 :Bool;
  note @2 :Text;
}

struct FriendList {
  playerId @0 :UInt64;
  friends @1 :List(FriendEntry);
}

struct GuildMember {
  memberId @0 :UInt64;
  rank @1 :UInt8;      # 0=会长,1=官员,2=成员
  online @2 :Bool;
}

struct GuildInfo {
  guildId @0 :UInt32;
  name @1 :Text;
  members @2 :List(GuildMember);
}

struct PartyUpdate {
  partyId @0 :UInt64;
  leaderId @1 :UInt64;
  members @2 :List(UInt64);
}

struct ChatBroadcast {
  channel @0 :ChatChannel;
  fromId @1 :UInt64;
  fromName @2 :Text;
  payload @3 :Text;
}

struct AchievementUpdate {
  playerId @0 :UInt64;
  achId @1 :UInt32;
  unlocked @2 :Bool;
}

struct ReputationUpdate {
  playerId @0 :UInt64;
  faction @1 :UInt16;   # 声望阵营 ID
  delta @2 :Int32;
  newValue @3 :Int32;
}
