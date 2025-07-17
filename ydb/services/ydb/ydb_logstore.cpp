#include "ydb_logstore.h"

#include <ydb/core/grpc_services/service_logstore.h>
#include <ydb/core/grpc_services/base/base.h>

#include <ydb/core/grpc_services/grpc_helper.h>

namespace NKikimr::NGRpcService {

void TGRpcYdbLogStoreService::SetupIncomingRequests(NYdbGrpc::TLoggerPtr logger) {
    using namespace Ydb;
    auto getCounterBlock = CreateCounterCb(Counters_, ActorSystem_);

#ifdef ADD_REQUEST
#error ADD_REQUEST macro already defined
#endif
#define ADD_REQUEST(NAME, CB, AUDIT_MODE_FLAGS) \
    MakeIntrusive<TGRpcRequest<LogStore::NAME##Request, LogStore::NAME##Response, TGRpcYdbLogStoreService>> \
        (this, &Service_, CQ_, [this](NYdbGrpc::IRequestContextBase *ctx) {                                 \
            NGRpcService::ReportGrpcReqToMon(*ActorSystem_, ctx->GetPeer());                                \
            ActorSystem_->Send(GRpcRequestProxyId_,                                                         \
                new TGrpcRequestOperationCall<LogStore::NAME##Request, LogStore::NAME##Response>            \
                    (ctx, &CB, TRequestAuxSettings{.AuditModeFlags = AUDIT_MODE_FLAGS}));                   \
        }, &Ydb::LogStore::V1::LogStoreService::AsyncService::Request ## NAME,                              \
        #NAME, logger, getCounterBlock("logstore", #NAME))->Run();

    ADD_REQUEST(CreateLogStore, DoCreateLogStoreRequest, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(DescribeLogStore, DoDescribeLogStoreRequest, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(DropLogStore, DoDropLogStoreRequest, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(AlterLogStore, DoAlterLogStoreRequest, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)

    ADD_REQUEST(CreateLogTable, DoCreateLogTableRequest, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(DescribeLogTable, DoDescribeLogTableRequest, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(DropLogTable, DoDropLogTableRequest, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(AlterLogTable, DoAlterLogTableRequest, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)

#undef ADD_REQUEST
}

}
