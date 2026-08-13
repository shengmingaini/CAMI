# CAMI 战斗/技能 — Cap'n Proto 统一协议 (阶段 A)
# 转写自 proto/flatbuffers/combat.fbs。
# 客户端发施法意图; 服务器权威结算后广播结果。
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xcA11c0de00000005;

using Common = import "common.capnp";

# 客户端 -> 服务器: 施法意图 (高频但可靠, 需服务器判定)
struct SkillCastIntent {
  entityId @0 :UInt64;      # 施法者 (= 自身 playerId)
  skillId @1 :UInt32;
  targetId @2 :UInt64;      # 0 = 无目标/自身
  aimPos @3 :Common.Vec3;   # 指向坐标
  clientTs @4 :UInt64;      # 客户端施法时间戳 (延迟补偿)
}

# 服务器 -> 客户端: 施法结果
struct SkillCastResult {
  intentSeq @0 :UInt32;     # 关联 SkillCastIntent 的 requestId
  result @1 :Common.SkillResultCode;
  castTs @2 :UInt64;        # 服务器判定时间戳
}

# 伤害事件 (逐次命中; PVP 走批次结算)
struct DamageEvent {
  sourceId @0 :UInt64;
  targetId @1 :UInt64;
  amount @2 :Int32;
  dtype @3 :Common.DamageType;
  isCrit @4 :Bool;
  serverTs @5 :UInt64;
}

# Buff 应用/移除事件
struct BuffEvent {
  targetId @0 :UInt64;
  buffId @1 :UInt32;
  apply @2 :Bool;           # true=应用, false=移除
  remainingMs @3 :UInt32;
  stacks @4 :UInt8;
}

# 战斗结果汇总 (击杀/死亡/脱战等广播给相关 AOI 作用域)
struct CombatResult {
  subjectId @0 :UInt64;
  dead @1 :Bool;
  killerId @2 :UInt64;      # dead=true 时有效
}
