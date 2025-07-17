#include "ydb_scheme.h"

#include <ydb/core/grpc_services/grpc_helper.h>

#include <ydb/core/grpc_services/service_scheme.h>
#include <ydb/core/grpc_services/base/base.h>

namespace NKikimr {
namespace NGRpcService {

void TGRpcYdbSchemeService::SetupIncomingRequests(NYdbGrpc::TLoggerPtr logger) {
    auto getCounterBlock = CreateCounterCb(Counters_, ActorSystem_);

#ifdef ADD_REQUEST
#error ADD_REQUEST macro already defined
#endif
#define ADD_REQUEST(NAME, CB, AUDIT_MODE_FLAGS) \
    MakeIntrusive<TGRpcRequest<Ydb::Scheme::NAME##Request, Ydb::Scheme::NAME##Response, TGRpcYdbSchemeService>>       \
        (this, &Service_, CQ_,                                                                                        \
            [this](NYdbGrpc::IRequestContextBase *ctx) {                                                              \
                NGRpcService::ReportGrpcReqToMon(*ActorSystem_, ctx->GetPeer());                                      \
                ActorSystem_->Send(GRpcRequestProxyId_,                                                               \
                    new TGrpcRequestOperationCall<Ydb::Scheme::NAME##Request, Ydb::Scheme::NAME##Response>            \
                        (ctx, &CB, TRequestAuxSettings{RLSWITCH(TRateLimiterMode::Rps), nullptr, AUDIT_MODE_FLAGS})); \
            }, &Ydb::Scheme::V1::SchemeService::AsyncService::Request ## NAME,                                        \
            #NAME, logger, getCounterBlock("scheme", #NAME))->Run();

    ADD_REQUEST(MakeDirectory, DoMakeDirectoryRequest, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(RemoveDirectory, DoRemoveDirectoryRequest, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(ListDirectory, DoListDirectoryRequest, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(DescribePath, DoDescribePathRequest, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(ModifyPermissions, DoModifyPermissionsRequest, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
#undef ADD_REQUEST
}

} // namespace NGRpcService
} // namespace NKikimr
