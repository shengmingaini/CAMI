# CAMI 移动/朝向 全量快照 — Cap'n Proto 统一协议 (阶段 A)
# 转写自 proto/flatbuffers/movement.fbs。
# 高频逐帧移动走轨道 A 位压缩(UDP), 本文件仅可靠轨全量快照。
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xcA11c0de00000003;

using Common = import "common.capnp";

# 全量位置/朝向快照 (可靠轨下发: AOI 进入首发/服务器纠偏/预测误差回滚)
struct MovementSnapshot {
  entityId @0 :UInt64;
  pos @1 :Common.Vec3;
  yaw @2 :Float32;          # 弧度
  moveState @3 :Common.MoveState;
  serverTs @4 :UInt64;      # 服务器采样时间戳 (延迟补偿)
}

# 服务器权威纠偏 (预测误差超限时下发, 覆盖客户端预测)
struct Correction {
  entityId @0 :UInt64;
  corrected @1 :MovementSnapshot;
  predictedClientTs @2 :UInt64;
}
