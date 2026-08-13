# CAMI 共享类型与枚举 — Cap'n Proto 统一协议 (阶段 A)
# 转写自 proto/flatbuffers/common.fbs。零拷贝 + 紧凑 + 单构建链。
# 构建: capnp compile -oc++ proto/capnp/common.capnp -> gen/cami/common.capnp.h
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xcA11c0de00000001;

# ---- 基础几何 (struct 仅含标量, 内联零开销) ----
struct Vec3 {
  x @0 :Float32;
  y @1 :Float32;
  z @2 :Float32;
}

struct Quat {
  x @0 :Float32;
  y @1 :Float32;
  z @2 :Float32;
  w @3 :Float32;
}

# ---- 枚举 ----
enum MoveState :UInt8 {
  idle;
  walk;
  run;
  jump;
  swim;
  fall;
}

enum LodLevel :UInt8 {
  lod0;   # 0-30m  完整属性
  lod1;   # 30-80m 关键属性
  lod2;   # 80-200m 最小属性
}

enum DamageType :UInt8 {
  physical;
  magic;
  fire;
  frost;
  holy;
  shadow;
  poison;
  true;   # 真实伤害, 无视抗性
}

enum SkillResultCode :UInt8 {
  success;
  insufficientMana;
  onCooldown;
  outOfRange;
  interrupt;
  invalidTarget;
  silenced;
}

enum Gender :UInt8 {
  male;
  female;
  unknown;
}

enum ClassId :UInt8 {
  none;        # 非玩家实体 (Monster/NPC/DropItem)
  warrior;
  mage;
  priest;
  rogue;
  ranger;
}

# ---- 通用复用结构 ----
# 角色核心属性快照 (同步/纠偏; 高频轨道 A 不携此结构)
struct AttributeSnapshot {
  hp @0 :Int32;
  maxHp @1 :Int32;
  mana @2 :Int32;
  maxMana @3 :Int32;
  level @4 :UInt16;
  exp @5 :UInt32;
  pos @6 :Vec3;
  yaw @7 :Float32;    # 弧度
}

# Buff 快照
struct BuffSnapshot {
  buffId @0 :UInt32;
  remainingMs @1 :UInt32;
  stacks @2 :UInt8;
}

# 物品授予单元 (任务奖励/经济发放/邮件附件复用, 零 JSON)
struct ItemGrant {
  itemId @0 :UInt32;
  count @1 :UInt32;
}
