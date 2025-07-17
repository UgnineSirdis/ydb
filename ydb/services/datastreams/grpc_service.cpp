#include "grpc_service.h"

#include <ydb/core/grpc_services/service_datastreams.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/ticket_parser.h>
#include <ydb/core/grpc_services/grpc_helper.h>
#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/tx/scheme_board/cache.h>
#include <ydb/core/tx/scheme_board/events.h>

namespace {

using namespace NKikimr;

void YdsProcessAttr(const TSchemeBoardEvents::TDescribeSchemeResult& schemeData, NGRpcService::ICheckerIface* checker) {
    static const std::vector<TString> allowedAttributes = {"folder_id", "service_account_id", "database_id"};
    //full list of permissions for compatibility. remove old permissions later.
    static const TVector<TString> permissions = {
        "ydb.streams.write",
        "ydb.databases.list",
        "ydb.databases.create",
        "ydb.databases.connect"
    };
    TVector<std::pair<TString, TString>> attributes;
    attributes.reserve(schemeData.GetPathDescription().UserAttributesSize());
    for (const auto& attr : schemeData.GetPathDescription().GetUserAttributes()) {
        if (std::find(allowedAttributes.begin(), allowedAttributes.end(), attr.GetKey()) != allowedAttributes.end()) {
            attributes.emplace_back(attr.GetKey(), attr.GetValue());
        }
    }
    if (!attributes.empty()) {
        checker->SetEntries({{permissions, attributes}});
    }
}

}

namespace NKikimr::NGRpcService {

void TGRpcDataStreamsService::SetupIncomingRequests(NYdbGrpc::TLoggerPtr logger)
{
    auto getCounterBlock = CreateCounterCb(Counters_, ActorSystem_);
    using std::placeholders::_1;
    using std::placeholders::_2;

#ifdef ADD_REQUEST
#error ADD_REQUEST macro already defined
#endif
#define ADD_REQUEST(NAME, CB, ATTR, LIMIT_TYPE, AUDIT_MODE_FLAGS) \
    MakeIntrusive<TGRpcRequest<Ydb::DataStreams::V1::NAME##Request, Ydb::DataStreams::V1::NAME##Response, TGRpcDataStreamsService>> \
        (this, &Service_, CQ_,                                                                                                      \
            [this](NYdbGrpc::IRequestContextBase *ctx) {                                                                            \
                NGRpcService::ReportGrpcReqToMon(*ActorSystem_, ctx->GetPeer());                                                    \
                ActorSystem_->Send(GRpcRequestProxyId_,                                                                             \
                    new TGrpcRequestOperationCall<Ydb::DataStreams::V1::NAME##Request, Ydb::DataStreams::V1::NAME##Response>        \
                        (ctx, CB, TRequestAuxSettings{RLSWITCH(TRateLimiterMode::LIMIT_TYPE), ATTR, AUDIT_MODE_FLAGS}));            \
            }, &Ydb::DataStreams::V1::DataStreamsService::AsyncService::Request ## NAME,                                            \
            #NAME, logger, getCounterBlock("data_streams", #NAME))->Run();

    ADD_REQUEST(DescribeStream, DoDataStreamsDescribeStreamRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(CreateStream, DoDataStreamsCreateStreamRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(ListStreams, DoDataStreamsListStreamsRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(DeleteStream, DoDataStreamsDeleteStreamRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(ListShards, DoDataStreamsListShardsRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(PutRecord, DoDataStreamsPutRecordRequest, YdsProcessAttr, RuManual, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(PutRecords, DoDataStreamsPutRecordsRequest, YdsProcessAttr, RuManual, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(GetRecords, DoDataStreamsGetRecordsRequest, nullptr, RuManual, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(GetShardIterator, DoDataStreamsGetShardIteratorRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(SubscribeToShard, DoDataStreamsSubscribeToShardRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(DescribeLimits, DoDataStreamsDescribeLimitsRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(DescribeStreamSummary, DoDataStreamsDescribeStreamSummaryRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(DecreaseStreamRetentionPeriod, DoDataStreamsDecreaseStreamRetentionPeriodRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(IncreaseStreamRetentionPeriod, DoDataStreamsIncreaseStreamRetentionPeriodRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(UpdateShardCount, DoDataStreamsUpdateShardCountRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(UpdateStreamMode, DoDataStreamsUpdateStreamModeRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(RegisterStreamConsumer, DoDataStreamsRegisterStreamConsumerRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(DeregisterStreamConsumer, DoDataStreamsDeregisterStreamConsumerRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(DescribeStreamConsumer, DoDataStreamsDescribeStreamConsumerRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(ListStreamConsumers, DoDataStreamsListStreamConsumersRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(AddTagsToStream, DoDataStreamsAddTagsToStreamRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(DisableEnhancedMonitoring, DoDataStreamsDisableEnhancedMonitoringRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(EnableEnhancedMonitoring, DoDataStreamsEnableEnhancedMonitoringRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(ListTagsForStream, DoDataStreamsListTagsForStreamRequest, nullptr, Off, TAuditModeFlags::DatabaseApi)
    ADD_REQUEST(MergeShards, DoDataStreamsMergeShardsRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(RemoveTagsFromStream, DoDataStreamsRemoveTagsFromStreamRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(SplitShard, DoDataStreamsSplitShardRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(StartStreamEncryption, DoDataStreamsStartStreamEncryptionRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(StopStreamEncryption, DoDataStreamsStopStreamEncryptionRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(UpdateStream, DoDataStreamsUpdateStreamRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)
    ADD_REQUEST(SetWriteQuota, DoDataStreamsSetWriteQuotaRequest, nullptr, Off, TAuditModeFlags::DatabaseApi | TAuditModeFlags::ModifyingOrCriticalApi)

#undef ADD_REQUEST
}

}
