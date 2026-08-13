# CAMI 兴趣域 AOI 同步 — Cap'n Proto 统一协议 (阶段 A)
# 转写自 proto/flatbuffers/aoi.fbs。LOD 三级/属性压缩/差分压缩。
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xcA11c0de00000004;

using Common = import "common.capnp";
using Movement = import "movement.capnp";

enum EntityKind :UInt8 {
  player;
  monster;
  npc;
  pet;
  dropItem;
}

# 实体进入视野: 首发完整快照
struct AoiEnter {
  entityId @0 :UInt64;
  kind @1 :EntityKind;
  name @2 :Text;                       # 仅 Player/NPC 有
  classId @3 :Common.ClassId;          # 仅 Player
  move @4 :Movement.MovementSnapshot;  # 位置/朝向
  attr @5 :Common.AttributeSnapshot;   # LOD0 完整属性
}

# 实体离开视野
struct AoiLeave {
  entityId @0 :UInt64;
}

# 视野内增量更新 (差分: 仅变化量)
struct AoiUpdate {
  entityId @0 :UInt64;
  lod @1 :Common.LodLevel;
  move @2 :Movement.MovementSnapshot;  # 每级必带位置/朝向
  attr @3 :Common.AttributeSnapshot;   # 仅 LOD0 填充
}
