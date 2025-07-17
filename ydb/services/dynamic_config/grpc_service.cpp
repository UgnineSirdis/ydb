#include "grpc_service.h"

#include <ydb/core/grpc_services/service_dynamic_config.h>
#include <ydb/core/grpc_services/grpc_helper.h>
#include <ydb/core/grpc_services/base/base.h>

namespace NKikimr {
namespace NGRpcService {

void TGRpcDynamicConfigService::SetupIncomingRequests(NYdbGrpc::TLoggerPtr logger) {
    auto getCounterBlock = CreateCounterCb(Counters_, ActorSystem_);
    using namespace Ydb;

#ifdef ADD_REQUEST
#error ADD_REQUEST macro already defined
#endif
#define ADD_REQUEST(NAME, CB, AUDIT_MODE_FLAGS)                                                                         \
    MakeIntrusive<TGRpcRequest<DynamicConfig::NAME##Request, DynamicConfig::NAME##Response, TGRpcDynamicConfigService>> \
        (this, &Service_, CQ_,                                                                                          \
            [this](NYdbGrpc::IRequestContextBase *ctx) {                                                                \
                NGRpcService::ReportGrpcReqToMon(*ActorSystem_, ctx->GetPeer());                                        \
                ActorSystem_->Send(GRpcRequestProxyId_,                                                                 \
                    new TGrpcRequestOperationCall<DynamicConfig::NAME##Request, DynamicConfig::NAME##Response>          \
                        (ctx, &CB, TRequestAuxSettings{RLSWITCH(TRateLimiterMode::Rps), nullptr, AUDIT_MODE_FLAGS}));   \
            }, &DynamicConfig::V1::DynamicConfigService::AsyncService::Request ## NAME,                                 \
            #NAME, logger, getCounterBlock("console", #NAME))->Run();

    ADD_REQUEST(SetConfig, DoSetConfigRequest, TAuditModeFlags::ClusterApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(ReplaceConfig, DoReplaceConfigRequest, TAuditModeFlags::ClusterApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(DropConfig, DoDropConfigRequest, TAuditModeFlags::ClusterApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(AddVolatileConfig, DoAddVolatileConfigRequest, TAuditModeFlags::ClusterApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(RemoveVolatileConfig, DoRemoveVolatileConfigRequest, TAuditModeFlags::ClusterApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(GetConfig, DoGetConfigRequest, TAuditModeFlags::ClusterApi)
    ADD_REQUEST(GetMetadata, DoGetMetadataRequest, TAuditModeFlags::ClusterApi)
    ADD_REQUEST(GetNodeLabels, DoGetNodeLabelsRequest, TAuditModeFlags::ClusterApi)
    ADD_REQUEST(ResolveConfig, DoResolveConfigRequest, TAuditModeFlags::ClusterApi)
    ADD_REQUEST(ResolveAllConfig, DoResolveAllConfigRequest, TAuditModeFlags::ClusterApi)
    ADD_REQUEST(FetchStartupConfig, DoFetchStartupConfigRequest, TAuditModeFlags::ClusterApi)
    ADD_REQUEST(GetConfigurationVersion, DoGetConfigurationVersionRequest, TAuditModeFlags::ClusterApi)

#undef ADD_REQUEST
}

} // namespace NGRpcService
} // namespace NKikimr
