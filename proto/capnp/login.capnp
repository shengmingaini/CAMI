# CAMI 连接/握手/登录/心跳/迁移 — Cap'n Proto 统一协议 (阶段 A)
# 转写自 proto/flatbuffers/login.fbs。
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xcA11c0de00000002;

using Common = import "common.capnp";

# ---- 握手 (AES-128-GCM 会话密钥协商) ----
struct ClientHello {
  x25519Pub @0 :Data;    # 32B 客户端临时公钥
  clientNonce @1 :Data;  # 12B
  protoVersion @2 :UInt16;
}

struct ServerHello {
  x25519Pub @0 :Data;    # 32B 服务端临时公钥
  serverNonce @1 :Data;  # 12B
  token @2 :Data;        # 时间戳签名, 防重放
}

# ---- 登录 ----
struct LoginRequest {
  account @0 :Text;        # 账号 (或平台 openid)
  token @1 :Text;          # 鉴权令牌
  clientVersion @2 :Text;  # 客户端构建号
}

enum LoginResult :UInt8 {
  ok;
  badToken;
  banned;
  serverFull;       # 背压 RED 时
  versionMismatch;
}

struct CharListEntry {
  playerId @0 :UInt64;
  name @1 :Text;
  classId @2 :Common.ClassId;
  gender @3 :Common.Gender;
  level @4 :UInt16;
}

struct LoginResponse {
  result @0 :LoginResult;
  chars @1 :List(CharListEntry);
}

# ---- 进入世界 ----
struct EnterWorldRequest {
  playerId @0 :UInt64;
}

struct EnterWorldResponse {
  result @0 :LoginResult;
  sceneId @1 :UInt32;
  spawnPos @2 :Common.Vec3;
  spawnYaw @3 :Float32;
  snapshot @4 :Common.AttributeSnapshot;
}

# ---- 连接生命周期 ----
struct Heartbeat {
  clientTs @0 :UInt64;   # 客户端毫秒时间戳
}

enum DisconnectReason :UInt8 {
  clientInitiated;
  timeout;
  serverShutdown;
  kick;
  migrated;   # 连接迁移走 QUIC 连接迁移, 不走此路径
}

struct DisconnectNotify {
  reason @0 :DisconnectReason;
  detail @1 :Text;
}

# GameNode 宕机时, 网关将连接上下文迁移到新节点 (QUIC 连接 ID 不变)
struct MigrateNotify {
  newGatewayId @0 :UInt32;
  contextToken @1 :UInt64;
}
