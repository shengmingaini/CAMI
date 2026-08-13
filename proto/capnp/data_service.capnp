# CAMI Data Service Protocol — Cap'n Proto schema (阶段 A 里程碑)
# 统一协议首份文件, 转写自 proto/data_service.proto。
# Cap'n Proto 优点: 零拷贝 + 无序列化代码(直接内存布局) + 单构建链。
# 构建: capnp compile -oc++ proto/capnp/data_service.capnp -> gen/cami/data_service.capnp.h
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xc001cafecafecafe;

struct GetRequest {
  key @0 :Text;
}

struct GetResponse {
  value @1 :Data;
  found @2 :Bool;
}

struct PutRequest {
  key @3 :Text;
  value @4 :Data;
}

struct PutResponse {
  ok @5 :Bool;
}

struct CasPutRequest {
  key @6 :Text;
  value @7 :Data;
  expectedVersion @8 :UInt64;
}

struct CasPutResponse {
  ok @9 :Bool;
  conflict @10 :Bool;
  currentVersion @11 :UInt64;
}

struct BatchPutRequest {
  items @12 :List(PutRequest);
}

struct BatchPutResponse {
  count @13 :UInt32;
  allOk @14 :Bool;
}

struct DeleteRequest {
  key @15 :Text;
}

struct DeleteResponse {
  ok @16 :Bool;
}

# 版本化值 (事件溯源/快照复用)
struct VersionedValue {
  value @0 :Data;
  version @1 :UInt64;
}
