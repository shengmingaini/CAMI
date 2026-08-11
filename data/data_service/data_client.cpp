// data/data_service/data_client.cpp — [PRODUCTION] Data Service gRPC 客户端实现
// 仅在 CAMI_BUILD_MODULES=ON 编译 (vcpkg: grpc + protobuf)。
#include "data/data_service/data_client.h"

#ifdef CAMI_BUILD_MODULES

#include <chrono>

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>

namespace cami {
namespace data {
namespace data_service {

DataClient::DataClient(const std::string& target)
    : stub_(CAMI::Data::DataService::NewStub(
          grpc::CreateChannel(target, grpc::InsecureChannelCredentials()))) {}

std::tuple<bool, std::string, uint64_t> DataClient::Get(uint64_t player_id) {
    CAMI::Data::GetReq req;
    req.set_player_id(player_id);
    CAMI::Data::GetResp resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));  // 防 Data Service 无响应阻塞战斗线程
    auto status = stub_->Get(&ctx, req, &resp);
    if (!status.ok()) return {false, "", 0};
    return {resp.found(), resp.payload(), resp.version()};
}

uint64_t DataClient::Put(uint64_t player_id, const std::string& payload, bool write_through) {
    CAMI::Data::PutReq req;
    req.set_player_id(player_id);
    req.set_payload(payload);
    req.set_write_through(write_through);
    CAMI::Data::PutResp resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    auto status = stub_->Put(&ctx, req, &resp);
    if (!status.ok()) return 0;
    return resp.version();
}

std::pair<bool, uint64_t> DataClient::Cas(uint64_t player_id, const std::string& payload,
                                          uint64_t expected_version) {
    CAMI::Data::CasReq req;
    req.set_player_id(player_id);
    req.set_payload(payload);
    req.set_expected_version(expected_version);
    CAMI::Data::CasResp resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    auto status = stub_->Cas(&ctx, req, &resp);
    if (!status.ok()) return {false, expected_version};
    return {resp.ok(), resp.version()};
}

bool DataClient::Delete(uint64_t player_id) {
    CAMI::Data::DeleteReq req;
    req.set_player_id(player_id);
    CAMI::Data::DeleteResp resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    auto status = stub_->Delete(&ctx, req, &resp);
    return status.ok() && resp.ok();
}

}  // namespace data_service
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
