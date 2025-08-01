#include "client.h"

#include "config.h"
#include "private.h"

#include <yt/yt/client/api/client.h>
#include <yt/yt/client/api/transaction.h>
#include <yt/yt/client/api/rpc_proxy/transaction_impl.h>
#include <yt/yt/client/api/dynamic_table_transaction_mixin.h>
#include <yt/yt/client/api/queue_transaction_mixin.h>

#include <yt/yt/client/misc/method_helpers.h>

#include <yt/yt/client/ypath/public.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/security_client/public.h>

#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/net/address.h>
#include <yt/yt/core/net/local_address.h>

#include <yt/yt/core/rpc/dispatcher.h>
#include <yt/yt/core/rpc/helpers.h>

#include <library/cpp/yt/memory/ref.h>

namespace NYT::NClient::NFederated {

using namespace NApi;
using namespace NQueueClient;

////////////////////////////////////////////////////////////////////////////////

constinit const auto Logger = FederatedClientLogger;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TClient)

////////////////////////////////////////////////////////////////////////////////

TFuture<std::optional<std::string>> GetDataCenterByClient(const IClientPtr& client)
{
    TListNodeOptions options;
    options.MaxSize = 1;

    return client->ListNode(RpcProxiesPath, options)
        .Apply(BIND([] (const NYson::TYsonString& items) {
            auto itemsList = NYTree::ConvertTo<NYTree::IListNodePtr>(items);
            if (!itemsList->GetChildCount()) {
                return std::optional<std::string>();
            }
            auto host = itemsList->GetChildren()[0];
            return NNet::InferYPClusterFromHostName(host->GetValue<std::string>());
        }));
}

class TTransaction
    : public virtual ITransaction
    , public TDynamicTableTransactionMixin
    , public TQueueTransactionMixin
    , public IFederatedClientTransactionMixin
{
public:
    TTransaction(TClientPtr client, int clientIndex, ITransactionPtr underlying);

    TFuture<ITransactionPtr> StartTransaction(
        NTransactionClient::ETransactionType type,
        const TTransactionStartOptions& options = {}) override;

    TFuture<TUnversionedLookupRowsResult> LookupRows(
        const NYPath::TYPath& path,
        NTableClient::TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TLegacyKey>& keys,
        const TLookupRowsOptions& options = {}) override;

    TFuture<TSelectRowsResult> SelectRows(
        const std::string& query,
        const TSelectRowsOptions& options = {}) override;

    void ModifyRows(
        const NYPath::TYPath& path,
        NTableClient::TNameTablePtr nameTable,
        TSharedRange<TRowModification> modifications,
        const TModifyRowsOptions& options) override;

    using TQueueTransactionMixin::AdvanceQueueConsumer;
    TFuture<void> AdvanceQueueConsumer(
        const NYPath::TRichYPath& consumerPath,
        const NYPath::TRichYPath& queuePath,
        int partitionIndex,
        std::optional<i64> oldOffset,
        i64 newOffset,
        const TAdvanceQueueConsumerOptions& options) override;

    TFuture<TPushQueueProducerResult> PushQueueProducer(
        const NYPath::TRichYPath& producerPath,
        const NYPath::TRichYPath& queuePath,
        const TQueueProducerSessionId& sessionId,
        TQueueProducerEpoch epoch,
        NTableClient::TNameTablePtr nameTable,
        TSharedRange<NTableClient::TUnversionedRow> rows,
        const TPushQueueProducerOptions& options) override;

    TFuture<TPushQueueProducerResult> PushQueueProducer(
        const NYPath::TRichYPath& producerPath,
        const NYPath::TRichYPath& queuePath,
        const TQueueProducerSessionId& sessionId,
        TQueueProducerEpoch epoch,
        NTableClient::TNameTablePtr nameTable,
        const std::vector<TSharedRef>& serializedRows,
        const TPushQueueProducerOptions& options) override;

    TFuture<TTransactionFlushResult> Flush() override;

    TFuture<void> Ping(const TPrerequisitePingOptions& options = {}) override;

    TFuture<TTransactionCommitResult> Commit(const TTransactionCommitOptions& options = TTransactionCommitOptions()) override;

    TFuture<void> Abort(const TTransactionAbortOptions& options = {}) override;

    TFuture<TVersionedLookupRowsResult> VersionedLookupRows(
        const NYPath::TYPath&, NTableClient::TNameTablePtr,
        const TSharedRange<NTableClient::TUnversionedRow>&,
        const TVersionedLookupRowsOptions&) override;

    TFuture<std::vector<TUnversionedLookupRowsResult>> MultiLookupRows(
        const std::vector<TMultiLookupSubrequest>&,
        const TMultiLookupOptions&) override;

    TFuture<NYson::TYsonString> ExplainQuery(const std::string&, const TExplainQueryOptions&) override;

    TFuture<NYson::TYsonString> GetNode(const NYPath::TYPath&, const TGetNodeOptions&) override;

    TFuture<NYson::TYsonString> ListNode(const NYPath::TYPath&, const TListNodeOptions&) override;

    TFuture<bool> NodeExists(const NYPath::TYPath&, const TNodeExistsOptions&) override;

    TFuture<TPullRowsResult> PullRows(const NYPath::TYPath&, const TPullRowsOptions&) override;

    IClientPtr GetClient() const override
    {
        return Underlying_->GetClient();
    }

    NTransactionClient::ETransactionType GetType() const override
    {
        return Underlying_->GetType();
    }

    NTransactionClient::TTransactionId GetId() const override
    {
        return Underlying_->GetId();
    }

    NTransactionClient::TTimestamp GetStartTimestamp() const override
    {
        return Underlying_->GetStartTimestamp();
    }

    NTransactionClient::EAtomicity GetAtomicity() const override
    {
        return Underlying_->GetAtomicity();
    }

    NTransactionClient::EDurability GetDurability() const override
    {
        return Underlying_->GetDurability();
    }

    TDuration GetTimeout() const override
    {
        return Underlying_->GetTimeout();
    }

    void Detach() override
    {
        return Underlying_->Detach();
    }

    void RegisterAlienTransaction(const ITransactionPtr& transaction) override
    {
        return Underlying_->RegisterAlienTransaction(transaction);
    }

    IConnectionPtr GetConnection() override
    {
        return Underlying_->GetConnection();
    }

    void SubscribeCommitted(const TCommittedHandler& handler) override
    {
        Underlying_->SubscribeCommitted(handler);
    }

    void UnsubscribeCommitted(const TCommittedHandler& handler) override
    {
        Underlying_->UnsubscribeCommitted(handler);
    }

    void SubscribeAborted(const TAbortedHandler& handler) override
    {
        Underlying_->SubscribeAborted(handler);
    }

    void UnsubscribeAborted(const TAbortedHandler& handler) override
    {
        Underlying_->UnsubscribeAborted(handler);
    }

    std::optional<std::string> TryGetStickyProxyAddress() const override
    {
        const auto* derived = Underlying_->TryAs<NRpcProxy::TTransaction>();
        return !derived ? std::nullopt : derived->GetStickyProxyAddress();
    }

    UNIMPLEMENTED_METHOD(TFuture<void>, SetNode, (const NYPath::TYPath&, const NYson::TYsonString&, const TSetNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, MultisetAttributesNode, (const NYPath::TYPath&, const NYTree::IMapNodePtr&, const TMultisetAttributesNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, RemoveNode, (const NYPath::TYPath&, const TRemoveNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NCypressClient::TNodeId>, CreateNode, (const NYPath::TYPath&, NObjectClient::EObjectType, const TCreateNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TLockNodeResult>, LockNode, (const NYPath::TYPath&, NCypressClient::ELockMode, const TLockNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, UnlockNode, (const NYPath::TYPath&, const TUnlockNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NCypressClient::TNodeId>, CopyNode, (const NYPath::TYPath&, const NYPath::TYPath&, const TCopyNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NCypressClient::TNodeId>, MoveNode, (const NYPath::TYPath&, const NYPath::TYPath&, const TMoveNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NCypressClient::TNodeId>, LinkNode, (const NYPath::TYPath&, const NYPath::TYPath&, const TLinkNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ConcatenateNodes, (const std::vector<NYPath::TRichYPath>&, const NYPath::TRichYPath&, const TConcatenateNodesOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ExternalizeNode, (const NYPath::TYPath&, NObjectClient::TCellTag, const TExternalizeNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, InternalizeNode, (const NYPath::TYPath&, const TInternalizeNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NObjectClient::TObjectId>, CreateObject, (NObjectClient::EObjectType, const TCreateObjectOptions&));
    UNIMPLEMENTED_METHOD(TFuture<ITableReaderPtr>, CreateTableReader, (const NYPath::TRichYPath&, const TTableReaderOptions&));
    UNIMPLEMENTED_METHOD(TFuture<IFileReaderPtr>, CreateFileReader, (const NYPath::TYPath&, const TFileReaderOptions&));
    UNIMPLEMENTED_METHOD(TFuture<ITableWriterPtr>, CreateTableWriter, (const NYPath::TRichYPath&, const TTableWriterOptions&));
    UNIMPLEMENTED_METHOD(IFileWriterPtr, CreateFileWriter, (const NYPath::TRichYPath&, const TFileWriterOptions&));
    UNIMPLEMENTED_METHOD(IJournalReaderPtr, CreateJournalReader, (const NYPath::TYPath&, const TJournalReaderOptions&));
    UNIMPLEMENTED_METHOD(IJournalWriterPtr, CreateJournalWriter, (const NYPath::TYPath&, const TJournalWriterOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TDistributedWriteSessionWithCookies>, StartDistributedWriteSession, (const NYPath::TRichYPath&, const TDistributedWriteSessionStartOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, FinishDistributedWriteSession, (const TDistributedWriteSessionWithResults&, const TDistributedWriteSessionFinishOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, Abort, (const TPrerequisiteAbortOptions&));

private:
    const TClientPtr Client_;
    const int ClientIndex_;
    const ITransactionPtr Underlying_;

    void OnResult(const TErrorOr<void>& error);
};

DECLARE_REFCOUNTED_TYPE(TTransaction)

////////////////////////////////////////////////////////////////////////////////

enum class EClientPriority : ui8
{
    Local,
    Remote,
    Undefined,
};

DECLARE_REFCOUNTED_STRUCT(TClientDescription)

struct TClientDescription final
{
    TClientDescription(IClientPtr client, EClientPriority priority)
        : Client(std::move(client))
        , Priority(priority)
    { }

    IClientPtr Client;
    EClientPriority Priority;
    std::atomic<bool> HasErrors{false};
};

DEFINE_REFCOUNTED_TYPE(TClientDescription)

////////////////////////////////////////////////////////////////////////////////

class TClient
    : public IClient
{
public:
    TClient(
        const std::vector<IClientPtr>& underlyingClients,
        TFederationConfigPtr config);

    TFuture<TUnversionedLookupRowsResult> LookupRows(
        const NYPath::TYPath& path,
        NTableClient::TNameTablePtr nameTable,
        const TSharedRange<NTableClient::TLegacyKey>& keys,
        const TLookupRowsOptions& options = {}) override;
    TFuture<TSelectRowsResult> SelectRows(
        const std::string& query,
        const TSelectRowsOptions& options = {}) override;
    TFuture<std::vector<TUnversionedLookupRowsResult>> MultiLookupRows(
        const std::vector<TMultiLookupSubrequest>&,
        const TMultiLookupOptions&) override;
    TFuture<TVersionedLookupRowsResult> VersionedLookupRows(
        const NYPath::TYPath&, NTableClient::TNameTablePtr,
        const TSharedRange<NTableClient::TUnversionedRow>&,
        const TVersionedLookupRowsOptions&) override;
    TFuture<TPullRowsResult> PullRows(const NYPath::TYPath&, const TPullRowsOptions&) override;

    TFuture<NQueueClient::IQueueRowsetPtr> PullQueue(
        const NYPath::TRichYPath&,
        i64,
        int,
        const NQueueClient::TQueueRowBatchReadOptions&,
        const TPullQueueOptions&) override;
    TFuture<NQueueClient::IQueueRowsetPtr> PullQueueConsumer(
        const NYPath::TRichYPath&,
        const NYPath::TRichYPath&,
        std::optional<i64>,
        int,
        const NQueueClient::TQueueRowBatchReadOptions&,
        const TPullQueueConsumerOptions&) override;

    TFuture<ITransactionPtr> StartTransaction(
        NTransactionClient::ETransactionType type,
        const NApi::TTransactionStartOptions& options) override;

    TFuture<NYson::TYsonString> ExplainQuery(const std::string&, const TExplainQueryOptions&) override;

    TFuture<NYson::TYsonString> GetNode(const NYPath::TYPath&, const TGetNodeOptions&) override;
    TFuture<NYson::TYsonString> ListNode(const NYPath::TYPath&, const TListNodeOptions&) override;
    TFuture<bool> NodeExists(const NYPath::TYPath&, const TNodeExistsOptions&) override;

    TFuture<std::vector<TListQueueConsumerRegistrationsResult>> ListQueueConsumerRegistrations(const std::optional<NYPath::TRichYPath>&, const std::optional<NYPath::TRichYPath>&, const TListQueueConsumerRegistrationsOptions&) override;

    const NTabletClient::ITableMountCachePtr& GetTableMountCache() override;
    TFuture<std::vector<TTabletInfo>> GetTabletInfos(const NYPath::TYPath&, const std::vector<int>&, const TGetTabletInfosOptions&) override;

    TFuture<NChaosClient::TReplicationCardPtr> GetReplicationCard(NChaosClient::TReplicationCardId, const TGetReplicationCardOptions&) override;

    const NTransactionClient::ITimestampProviderPtr& GetTimestampProvider() override;

    ITransactionPtr AttachTransaction(NTransactionClient::TTransactionId, const TTransactionAttachOptions&) override;

    IPrerequisitePtr AttachPrerequisite(NPrerequisiteClient::TPrerequisiteId, const TPrerequisiteAttachOptions&) override;

    IConnectionPtr GetConnection() override
    {
        auto [client, _] = GetActiveClient();
        return client->GetConnection();
    }

    TFuture<std::optional<std::string>> GetClusterName(bool fetchIfNull) override
    {
        auto [client, _] = GetActiveClient();
        return client->GetClusterName(fetchIfNull);
    }

    void Terminate() override
    { }

    TFuture<TGetCurrentUserResultPtr> GetCurrentUser(const TGetCurrentUserOptions& options) override
    {
        auto [client, _] = GetActiveClient();
        return client->GetCurrentUser(options);
    }

    // IClientBase unsupported methods.
    UNIMPLEMENTED_METHOD(TFuture<void>, SetNode, (const NYPath::TYPath&, const NYson::TYsonString&, const TSetNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, MultisetAttributesNode, (const NYPath::TYPath&, const NYTree::IMapNodePtr&, const TMultisetAttributesNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, RemoveNode, (const NYPath::TYPath&, const TRemoveNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NCypressClient::TNodeId>, CreateNode, (const NYPath::TYPath&, NObjectClient::EObjectType, const TCreateNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TLockNodeResult>, LockNode, (const NYPath::TYPath&, NCypressClient::ELockMode, const TLockNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, UnlockNode, (const NYPath::TYPath&, const TUnlockNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NCypressClient::TNodeId>, CopyNode, (const NYPath::TYPath&, const NYPath::TYPath&, const TCopyNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NCypressClient::TNodeId>, MoveNode, (const NYPath::TYPath&, const NYPath::TYPath&, const TMoveNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NCypressClient::TNodeId>, LinkNode, (const NYPath::TYPath&, const NYPath::TYPath&, const TLinkNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ConcatenateNodes, (const std::vector<NYPath::TRichYPath>&, const NYPath::TRichYPath&, const TConcatenateNodesOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ExternalizeNode, (const NYPath::TYPath&, NObjectClient::TCellTag, const TExternalizeNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, InternalizeNode, (const NYPath::TYPath&, const TInternalizeNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NObjectClient::TObjectId>, CreateObject, (NObjectClient::EObjectType, const TCreateObjectOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TQueryResult>, GetQueryResult, (NQueryTrackerClient::TQueryId, i64, const TGetQueryResultOptions&));

    // IClient unsupported methods.
    UNIMPLEMENTED_METHOD(TFuture<void>, RegisterQueueConsumer, (const NYPath::TRichYPath&, const NYPath::TRichYPath&, bool, const TRegisterQueueConsumerOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, UnregisterQueueConsumer, (const NYPath::TRichYPath&, const NYPath::TRichYPath&, const TUnregisterQueueConsumerOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TCreateQueueProducerSessionResult>, CreateQueueProducerSession, (const NYPath::TRichYPath&, const NYPath::TRichYPath&, const TQueueProducerSessionId&, const TCreateQueueProducerSessionOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, RemoveQueueProducerSession, (const NYPath::TRichYPath&, const NYPath::TRichYPath&, const TQueueProducerSessionId&, const TRemoveQueueProducerSessionOptions&));

    UNIMPLEMENTED_METHOD(const NChaosClient::IReplicationCardCachePtr&, GetReplicationCardCache, ());
    UNIMPLEMENTED_METHOD(TFuture<void>, MountTable, (const NYPath::TYPath&, const TMountTableOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, UnmountTable, (const NYPath::TYPath&, const TUnmountTableOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, RemountTable, (const NYPath::TYPath&, const TRemountTableOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, FreezeTable, (const NYPath::TYPath&, const TFreezeTableOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, UnfreezeTable, (const NYPath::TYPath&, const TUnfreezeTableOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, CancelTabletTransition, (NTabletClient::TTabletId, const TCancelTabletTransitionOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ReshardTable, (const NYPath::TYPath&, const std::vector<NTableClient::TUnversionedOwningRow>&, const TReshardTableOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ReshardTable, (const NYPath::TYPath&, int, const TReshardTableOptions&));
    UNIMPLEMENTED_METHOD(TFuture<std::vector<NTabletClient::TTabletActionId>>, ReshardTableAutomatic, (const NYPath::TYPath&, const TReshardTableAutomaticOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, TrimTable, (const NYPath::TYPath&, int, i64, const TTrimTableOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, AlterTable, (const NYPath::TYPath&, const TAlterTableOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, AlterTableReplica, (NTabletClient::TTableReplicaId, const TAlterTableReplicaOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, AlterReplicationCard, (NChaosClient::TReplicationCardId, const TAlterReplicationCardOptions&));
    UNIMPLEMENTED_METHOD(TFuture<IPrerequisitePtr>, StartChaosLease, (const TChaosLeaseStartOptions&));
    UNIMPLEMENTED_METHOD(TFuture<IPrerequisitePtr>, AttachChaosLease, (NChaosClient::TChaosLeaseId, const TChaosLeaseAttachOptions&));
    UNIMPLEMENTED_METHOD(TFuture<std::vector<NTabletClient::TTableReplicaId>>, GetInSyncReplicas, (const NYPath::TYPath&, const NTableClient::TNameTablePtr&, const TSharedRange<NTableClient::TUnversionedRow>&, const TGetInSyncReplicasOptions&));
    UNIMPLEMENTED_METHOD(TFuture<std::vector<NTabletClient::TTableReplicaId>>, GetInSyncReplicas, (const NYPath::TYPath&, const TGetInSyncReplicasOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TGetTabletErrorsResult>, GetTabletErrors, (const NYPath::TYPath&, const TGetTabletErrorsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<std::vector<NTabletClient::TTabletActionId>>, BalanceTabletCells, (const std::string&, const std::vector<NYPath::TYPath>&, const TBalanceTabletCellsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TSkynetSharePartsLocationsPtr>, LocateSkynetShare, (const NYPath::TRichYPath&, const TLocateSkynetShareOptions&));
    UNIMPLEMENTED_METHOD(TFuture<std::vector<NTableClient::TColumnarStatistics>>, GetColumnarStatistics, (const std::vector<NYPath::TRichYPath>&, const TGetColumnarStatisticsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TMultiTablePartitions>, PartitionTables, (const std::vector<NYPath::TRichYPath>&, const TPartitionTablesOptions&));
    UNIMPLEMENTED_METHOD(TFuture<ITablePartitionReaderPtr>, CreateTablePartitionReader, (const TTablePartitionCookiePtr&, const TReadTablePartitionOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NYson::TYsonString>, GetTablePivotKeys, (const NYPath::TYPath&, const TGetTablePivotKeysOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, CreateTableBackup, (const TBackupManifestPtr&, const TCreateTableBackupOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, RestoreTableBackup, (const TBackupManifestPtr&, const TRestoreTableBackupOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, TruncateJournal, (const NYPath::TYPath&, i64, const TTruncateJournalOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TGetFileFromCacheResult>, GetFileFromCache, (const TString&, const TGetFileFromCacheOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TPutFileToCacheResult>, PutFileToCache, (const NYPath::TYPath&, const TString&, const TPutFileToCacheOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, AddMember, (const std::string&, const std::string&, const TAddMemberOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, RemoveMember, (const std::string&, const std::string&, const TRemoveMemberOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TCheckPermissionResponse>, CheckPermission, (const std::string&, const NYPath::TYPath&, NYTree::EPermission, const TCheckPermissionOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TCheckPermissionByAclResult>, CheckPermissionByAcl, (const std::optional<std::string>&, NYTree::EPermission, NYTree::INodePtr, const TCheckPermissionByAclOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, TransferAccountResources, (const std::string&, const std::string&, NYTree::INodePtr, const TTransferAccountResourcesOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, TransferPoolResources, (const TString&, const TString&, const TString&, NYTree::INodePtr, const TTransferPoolResourcesOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NScheduler::TOperationId>, StartOperation, (NScheduler::EOperationType, const NYson::TYsonString&, const TStartOperationOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, AbortOperation, (const NScheduler::TOperationIdOrAlias&, const TAbortOperationOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, SuspendOperation, (const NScheduler::TOperationIdOrAlias&, const TSuspendOperationOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ResumeOperation, (const NScheduler::TOperationIdOrAlias&, const TResumeOperationOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, CompleteOperation, (const NScheduler::TOperationIdOrAlias&, const TCompleteOperationOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, UpdateOperationParameters, (const NScheduler::TOperationIdOrAlias&, const NYson::TYsonString&, const TUpdateOperationParametersOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, PatchOperationSpec, (const NScheduler::TOperationIdOrAlias&, const NScheduler::TSpecPatchList&, const TPatchOperationSpecOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TOperation>, GetOperation, (const NScheduler::TOperationIdOrAlias&, const TGetOperationOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, DumpJobContext, (NJobTrackerClient::TJobId, const NYPath::TYPath&, const TDumpJobContextOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NConcurrency::IAsyncZeroCopyInputStreamPtr>, GetJobInput, (NJobTrackerClient::TJobId, const TGetJobInputOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NYson::TYsonString>, GetJobInputPaths, (NJobTrackerClient::TJobId, const TGetJobInputPathsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NYson::TYsonString>, GetJobSpec, (NJobTrackerClient::TJobId, const TGetJobSpecOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TGetJobStderrResponse>, GetJobStderr, (const NScheduler::TOperationIdOrAlias&, NJobTrackerClient::TJobId, const TGetJobStderrOptions&));
    UNIMPLEMENTED_METHOD(TFuture<std::vector<TJobTraceEvent>>, GetJobTrace, (const NScheduler::TOperationIdOrAlias&, const TGetJobTraceOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TSharedRef>, GetJobFailContext, (const NScheduler::TOperationIdOrAlias&, NJobTrackerClient::TJobId, const TGetJobFailContextOptions&));
    UNIMPLEMENTED_METHOD(TFuture<std::vector<TOperationEvent>>, ListOperationEvents, (const NScheduler::TOperationIdOrAlias&, const TListOperationEventsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TListOperationsResult>, ListOperations, (const TListOperationsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TListJobsResult>, ListJobs, (const NScheduler::TOperationIdOrAlias&, const TListJobsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NYson::TYsonString>, GetJob, (const NScheduler::TOperationIdOrAlias&, NJobTrackerClient::TJobId, const TGetJobOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, AbandonJob, (NJobTrackerClient::TJobId, const TAbandonJobOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TPollJobShellResponse>, PollJobShell, (NJobTrackerClient::TJobId, const std::optional<TString>&, const NYson::TYsonString&, const TPollJobShellOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, AbortJob, (NJobTrackerClient::TJobId, const TAbortJobOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, DumpJobProxyLog, (NJobTrackerClient::TJobId, NJobTrackerClient::TOperationId, const NYPath::TYPath&, const TDumpJobProxyLogOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TClusterMeta>, GetClusterMeta, (const TGetClusterMetaOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, CheckClusterLiveness, (const TCheckClusterLivenessOptions&));
    UNIMPLEMENTED_METHOD(TFuture<int>, BuildSnapshot, (const TBuildSnapshotOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TCellIdToSnapshotIdMap>, BuildMasterSnapshots, (const TBuildMasterSnapshotsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TCellIdToConsistentStateMap>, GetMasterConsistentState, (const TGetMasterConsistentStateOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ExitReadOnly, (NObjectClient::TCellId, const TExitReadOnlyOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, MasterExitReadOnly, (const TMasterExitReadOnlyOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, DiscombobulateNonvotingPeers, (NObjectClient::TCellId, const TDiscombobulateNonvotingPeersOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, SwitchLeader, (NObjectClient::TCellId, const std::string&, const TSwitchLeaderOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ResetStateHash, (NObjectClient::TCellId, const TResetStateHashOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, GCCollect, (const TGCCollectOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, KillProcess, (const std::string&, const TKillProcessOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TString>, WriteCoreDump, (const std::string&, const TWriteCoreDumpOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TGuid>, WriteLogBarrier, (const std::string&, const TWriteLogBarrierOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TString>, WriteOperationControllerCoreDump, (NJobTrackerClient::TOperationId, const TWriteOperationControllerCoreDumpOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, HealExecNode, (const std::string&, const THealExecNodeOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, SuspendCoordinator, (NObjectClient::TCellId, const TSuspendCoordinatorOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ResumeCoordinator, (NObjectClient::TCellId, const TResumeCoordinatorOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, MigrateReplicationCards, (NObjectClient::TCellId, const TMigrateReplicationCardsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, SuspendChaosCells, (const std::vector<NObjectClient::TCellId>&, const TSuspendChaosCellsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ResumeChaosCells, (const std::vector<NObjectClient::TCellId>&, const TResumeChaosCellsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, SuspendTabletCells, (const std::vector<NObjectClient::TCellId>&, const TSuspendTabletCellsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, ResumeTabletCells, (const std::vector<NObjectClient::TCellId>&, const TResumeTabletCellsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, UpdateChaosTableReplicaProgress, (NChaosClient::TReplicaId, const TUpdateChaosTableReplicaProgressOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TMaintenanceIdPerTarget>, AddMaintenance, (EMaintenanceComponent, const std::string&, EMaintenanceType, const TString&, const TAddMaintenanceOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TMaintenanceCountsPerTarget>, RemoveMaintenance, (EMaintenanceComponent, const std::string&, const TMaintenanceFilter&, const TRemoveMaintenanceOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TDisableChunkLocationsResult>, DisableChunkLocations, (const std::string&, const std::vector<TGuid>&, const TDisableChunkLocationsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TDestroyChunkLocationsResult>, DestroyChunkLocations, (const std::string&, bool, const std::vector<TGuid>&, const TDestroyChunkLocationsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TResurrectChunkLocationsResult>, ResurrectChunkLocations, (const std::string&, const std::vector<TGuid>&, const TResurrectChunkLocationsOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TRequestRestartResult>, RequestRestart, (const std::string&, const TRequestRestartOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TCollectCoverageResult>, CollectCoverage, (const std::string&, const TCollectCoverageOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, SetUserPassword, (const std::string&, const TString&, const TString&, const TSetUserPasswordOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TIssueTokenResult>, IssueToken, (const std::string&, const TString&, const TIssueTokenOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, RevokeToken, (const std::string&, const TString&, const TString&, const TRevokeTokenOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TListUserTokensResult>, ListUserTokens, (const std::string&, const TString&, const TListUserTokensOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NQueryTrackerClient::TQueryId>, StartQuery, (NQueryTrackerClient::EQueryEngine, const TString&, const TStartQueryOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, AbortQuery, (NQueryTrackerClient::TQueryId, const TAbortQueryOptions&));
    UNIMPLEMENTED_METHOD(TFuture<IUnversionedRowsetPtr>, ReadQueryResult, (NQueryTrackerClient::TQueryId, i64, const TReadQueryResultOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TQuery>, GetQuery, (NQueryTrackerClient::TQueryId, const TGetQueryOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TListQueriesResult>, ListQueries, (const TListQueriesOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, AlterQuery, (NQueryTrackerClient::TQueryId, const TAlterQueryOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TGetQueryTrackerInfoResult>, GetQueryTrackerInfo, (const TGetQueryTrackerInfoOptions&));
    UNIMPLEMENTED_METHOD(TFuture<NBundleControllerClient::TBundleConfigDescriptorPtr>, GetBundleConfig, (const std::string&, const NBundleControllerClient::TGetBundleConfigOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, SetBundleConfig, (const std::string&, const NBundleControllerClient::TBundleTargetConfigPtr&, const NBundleControllerClient::TSetBundleConfigOptions&));
    UNIMPLEMENTED_METHOD(TFuture<ITableReaderPtr>, CreateTableReader, (const NYPath::TRichYPath&, const TTableReaderOptions&));
    UNIMPLEMENTED_METHOD(TFuture<ITableWriterPtr>, CreateTableWriter, (const NYPath::TRichYPath&, const TTableWriterOptions&));
    UNIMPLEMENTED_METHOD(TFuture<IFileReaderPtr>, CreateFileReader, (const NYPath::TYPath&, const TFileReaderOptions&));
    UNIMPLEMENTED_METHOD(IFileWriterPtr, CreateFileWriter, (const NYPath::TRichYPath&, const TFileWriterOptions&));
    UNIMPLEMENTED_METHOD(IJournalReaderPtr, CreateJournalReader, (const NYPath::TYPath&, const TJournalReaderOptions&));
    UNIMPLEMENTED_METHOD(IJournalWriterPtr, CreateJournalWriter, (const NYPath::TYPath&, const TJournalWriterOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TGetPipelineSpecResult>, GetPipelineSpec, (const NYPath::TYPath&, const TGetPipelineSpecOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TSetPipelineSpecResult>, SetPipelineSpec, (const NYPath::TYPath&, const NYson::TYsonString&, const TSetPipelineSpecOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TGetPipelineDynamicSpecResult>, GetPipelineDynamicSpec, (const NYPath::TYPath&, const TGetPipelineDynamicSpecOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TSetPipelineDynamicSpecResult>, SetPipelineDynamicSpec, (const NYPath::TYPath&, const NYson::TYsonString&, const TSetPipelineDynamicSpecOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, StartPipeline, (const NYPath::TYPath&, const TStartPipelineOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, StopPipeline, (const NYPath::TYPath&, const TStopPipelineOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, PausePipeline, (const NYPath::TYPath&, const TPausePipelineOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TPipelineState>, GetPipelineState, (const NYPath::TYPath&, const TGetPipelineStateOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TGetFlowViewResult>, GetFlowView, (const NYPath::TYPath&, const NYPath::TYPath&, const TGetFlowViewOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TFlowExecuteResult>, FlowExecute, (const NYPath::TYPath&, const TString&, const NYson::TYsonString&, const TFlowExecuteOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TDistributedWriteSessionWithCookies>, StartDistributedWriteSession, (const NYPath::TRichYPath&, const TDistributedWriteSessionStartOptions&));
    UNIMPLEMENTED_METHOD(TFuture<void>, FinishDistributedWriteSession, (const TDistributedWriteSessionWithResults&, const TDistributedWriteSessionFinishOptions&));
    UNIMPLEMENTED_METHOD(TFuture<ITableFragmentWriterPtr>, CreateTableFragmentWriter, (const TSignedWriteFragmentCookiePtr&, const TTableFragmentWriterOptions&));
    UNIMPLEMENTED_METHOD(TFuture<TSignedShuffleHandlePtr>, StartShuffle, (const std::string& , int, NObjectClient::TTransactionId, const TStartShuffleOptions&));
    UNIMPLEMENTED_METHOD(TFuture<IRowBatchReaderPtr>, CreateShuffleReader, (const TSignedShuffleHandlePtr&, int, std::optional<TIndexRange>, const TShuffleReaderOptions&));
    UNIMPLEMENTED_METHOD(TFuture<IRowBatchWriterPtr>, CreateShuffleWriter, (const TSignedShuffleHandlePtr&, const std::string&, std::optional<int>, const TShuffleWriterOptions&));

private:
    friend class TTransaction;

    struct TActiveClientInfo
    {
        IClientPtr Client;
        int ClientIndex;
    };

    template <class T>
    TFuture<T> DoCall(int retryAttemptCount, const TCallback<TFuture<T>(const IClientPtr&, int)>& callee);
    void HandleError(const TErrorOr<void>& error, int clientIndex);

    void UpdateActiveClient();
    TActiveClientInfo GetActiveClient();

    void CheckClustersHealth();

private:
    const TFederationConfigPtr Config_;
    const NConcurrency::TPeriodicExecutorPtr Executor_;
    const TString LocalDatacenter_;

    std::vector<TClientDescriptionPtr> UnderlyingClients_;
    IClientPtr ActiveClient_;
    std::atomic<int> ActiveClientIndex_;

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, Lock_);
};

DECLARE_REFCOUNTED_TYPE(TTransaction)

////////////////////////////////////////////////////////////////////////////////

TTransaction::TTransaction(TClientPtr client, int clientIndex, ITransactionPtr underlying)
    : Client_(std::move(client))
    , ClientIndex_(clientIndex)
    , Underlying_(std::move(underlying))
{ }

void TTransaction::OnResult(const TErrorOr<void>& error)
{
    if (!error.IsOK()) {
        Client_->HandleError(error, ClientIndex_);
    }
}

#define TRANSACTION_METHOD_IMPL(ResultType, MethodName, Args)                                                           \
TFuture<ResultType> TTransaction::MethodName(Y_METHOD_USED_ARGS_DECLARATION(Args))                                      \
{                                                                                                                       \
    auto future = Underlying_->MethodName(Y_PASS_METHOD_USED_ARGS(Args));                                               \
    future.Subscribe(BIND(&TTransaction::OnResult, MakeStrong(this)));                                                  \
    return future;                                                                                                      \
} Y_SEMICOLON_GUARD

TRANSACTION_METHOD_IMPL(TUnversionedLookupRowsResult, LookupRows, (const NYPath::TYPath&, NTableClient::TNameTablePtr, const TSharedRange<NTableClient::TUnversionedRow>&, const TLookupRowsOptions&));
TRANSACTION_METHOD_IMPL(TSelectRowsResult, SelectRows, (const std::string&, const TSelectRowsOptions&));
TRANSACTION_METHOD_IMPL(void, Ping, (const NApi::TPrerequisitePingOptions&));
TRANSACTION_METHOD_IMPL(TTransactionCommitResult, Commit, (const TTransactionCommitOptions&));
TRANSACTION_METHOD_IMPL(void, Abort, (const TTransactionAbortOptions&));
TRANSACTION_METHOD_IMPL(TVersionedLookupRowsResult, VersionedLookupRows, (const NYPath::TYPath&, NTableClient::TNameTablePtr, const TSharedRange<NTableClient::TUnversionedRow>&, const TVersionedLookupRowsOptions&));
TRANSACTION_METHOD_IMPL(std::vector<TUnversionedLookupRowsResult>, MultiLookupRows, (const std::vector<TMultiLookupSubrequest>&, const TMultiLookupOptions&));
TRANSACTION_METHOD_IMPL(TPullRowsResult, PullRows, (const NYPath::TYPath&, const TPullRowsOptions&));
TRANSACTION_METHOD_IMPL(void, AdvanceQueueConsumer, (const NYPath::TRichYPath&, const NYPath::TRichYPath&, int, std::optional<i64>, i64, const TAdvanceQueueConsumerOptions&));
TRANSACTION_METHOD_IMPL(TPushQueueProducerResult, PushQueueProducer, (const NYPath::TRichYPath&, const NYPath::TRichYPath&, const TQueueProducerSessionId&, TQueueProducerEpoch, NTableClient::TNameTablePtr, TSharedRange<NTableClient::TUnversionedRow>, const TPushQueueProducerOptions&));
TRANSACTION_METHOD_IMPL(TPushQueueProducerResult, PushQueueProducer, (const NYPath::TRichYPath&, const NYPath::TRichYPath&, const TQueueProducerSessionId&, TQueueProducerEpoch, NTableClient::TNameTablePtr, const std::vector<TSharedRef>&, const TPushQueueProducerOptions&));
TRANSACTION_METHOD_IMPL(NYson::TYsonString, ExplainQuery, (const std::string&, const TExplainQueryOptions&));
TRANSACTION_METHOD_IMPL(NYson::TYsonString, GetNode, (const NYPath::TYPath&, const TGetNodeOptions&));
TRANSACTION_METHOD_IMPL(NYson::TYsonString, ListNode, (const NYPath::TYPath&, const TListNodeOptions&));
TRANSACTION_METHOD_IMPL(bool, NodeExists, (const NYPath::TYPath&, const TNodeExistsOptions&));

void TTransaction::ModifyRows(
    const NYPath::TYPath& path,
    NTableClient::TNameTablePtr nameTable,
    TSharedRange<TRowModification> modifications,
    const TModifyRowsOptions& options)
{
    Underlying_->ModifyRows(path, nameTable, modifications, options);
}

TFuture<TTransactionFlushResult> TTransaction::Flush()
{
    auto future = Underlying_->Flush();
    future.Subscribe(BIND(&TTransaction::OnResult, MakeStrong(this)));
    return future;
}

TFuture<ITransactionPtr> TTransaction::StartTransaction(
    NTransactionClient::ETransactionType type,
    const TTransactionStartOptions& options)
{
    return Underlying_->StartTransaction(type, options).ApplyUnique(BIND(
        [this, this_ = MakeStrong(this)] (TErrorOr<ITransactionPtr>&& result) -> TErrorOr<ITransactionPtr> {
            if (!result.IsOK()) {
                Client_->HandleError(result, ClientIndex_);
                return result;
            } else {
                return {New<TTransaction>(Client_, ClientIndex_, result.Value())};
            }
        }));
}

DEFINE_REFCOUNTED_TYPE(TTransaction)

////////////////////////////////////////////////////////////////////////////////

TClient::TClient(const std::vector<IClientPtr>& underlyingClients, TFederationConfigPtr config)
    : Config_(std::move(config))
    , Executor_(New<NConcurrency::TPeriodicExecutor>(
        NRpc::TDispatcher::Get()->GetLightInvoker(),
        BIND(&TClient::CheckClustersHealth, MakeWeak(this)),
        Config_->ClusterHealthCheckPeriod))
    , LocalDatacenter_(NNet::GetLocalYPCluster())
{
    YT_VERIFY(!underlyingClients.empty());

    UnderlyingClients_.reserve(underlyingClients.size());
    for (const auto& client : underlyingClients) {
        UnderlyingClients_.push_back(New<TClientDescription>(client, EClientPriority::Undefined));
    }
    ActiveClient_ = UnderlyingClients_[0]->Client;
    ActiveClientIndex_ = 0;

    Executor_->Start();
}

void TClient::CheckClustersHealth()
{
    TCheckClusterLivenessOptions options;
    options.CheckCypressRoot = Config_->CheckCypressRoot;
    options.CheckTabletCellBundle = Config_->BundleName;

    std::vector<TFuture<void>> checks;
    checks.reserve(UnderlyingClients_.size());
    for (const auto& clientDescription : UnderlyingClients_) {
        checks.emplace_back(clientDescription->Client->CheckClusterLiveness(options));
    }

    for (int index = 0; index != std::ssize(checks); ++index) {
        const auto& check = checks[index];
        auto error = NConcurrency::WaitFor(check);
        YT_LOG_DEBUG_UNLESS(error.IsOK(), error, "Cluster %Qv is marked as unhealthy",
            UnderlyingClients_[index]->Client->GetConnection()->GetClusterName());
        UnderlyingClients_[index]->HasErrors = !error.IsOK()
            && !error.FindMatching(NSecurityClient::EErrorCode::AuthorizationError); // Ignore authorization errors.
    }

    for (int index = 0; index != std::ssize(UnderlyingClients_); ++index) {
        auto& client = UnderlyingClients_[index];
        // `Priority` accessed only from this thread so it is not require synchronization.
        if (client->Priority == EClientPriority::Undefined) {
            auto clientDatacenter = NConcurrency::WaitFor(GetDataCenterByClient(client->Client));
            if (clientDatacenter.IsOK()) {
                client->Priority = clientDatacenter.Value() == LocalDatacenter_
                    ? EClientPriority::Local
                    : EClientPriority::Remote;
            }
        }
    }
    // Compute better activeClientIndex.
    int betterClientIndex = ActiveClientIndex_.load();
    auto betterPriority = UnderlyingClients_[betterClientIndex]->HasErrors
        ? EClientPriority::Undefined
        : UnderlyingClients_[betterClientIndex]->Priority;

    for (int index  = 0; index != std::ssize(UnderlyingClients_); ++index) {
        const auto& client = UnderlyingClients_[index];
        if (!client->HasErrors && client->Priority < betterPriority) {
            betterClientIndex = index;
            betterPriority = client->Priority;
        }
    }

    if (ActiveClientIndex_ != betterClientIndex) {
        auto guard = NThreading::WriterGuard(Lock_);
        ActiveClient_ = UnderlyingClients_[betterClientIndex]->Client;
        ActiveClientIndex_ = betterClientIndex;
    }
}

template <class T>
TFuture<T> TClient::DoCall(int retryAttemptCount, const TCallback<TFuture<T>(const IClientPtr&, int)>& callee)
{
    auto [client, clientIndex] = GetActiveClient();
    return callee(client, clientIndex).ApplyUnique(BIND(
        [
            this,
            this_ = MakeStrong(this),
            retryAttemptCount,
            callee,
            clientIndex = clientIndex
        ] (TErrorOr<T>&& result) {
            if (!result.IsOK()) {
                HandleError(result, clientIndex);
                if (retryAttemptCount > 1) {
                    return DoCall<T>(retryAttemptCount - 1, callee);
                }
            }
            return MakeFuture(std::move(result));
        }));
}

TFuture<ITransactionPtr> TClient::StartTransaction(
    NTransactionClient::ETransactionType type,
    const NApi::TTransactionStartOptions& options)
{
    auto callee = BIND([this_ = MakeStrong(this), type, options] (const IClientPtr& client, int clientIndex) {
        return client->StartTransaction(type, options).ApplyUnique(BIND(
            [this_, clientIndex] (ITransactionPtr&& transaction) -> ITransactionPtr {
                return New<TTransaction>(std::move(this_), clientIndex, std::move(transaction));
            }));
    });

    return DoCall<ITransactionPtr>(Config_->ClusterRetryAttempts, callee);
}

#define CLIENT_METHOD_IMPL(ResultType, MethodName, Args)                                                                \
TFuture<ResultType> TClient::MethodName(Y_METHOD_USED_ARGS_DECLARATION(Args))                                           \
{                                                                                                                       \
    auto callee = BIND([Y_PASS_METHOD_USED_ARGS(Args)] (const IClientPtr& client, int /*clientIndex*/) {                \
        return client->MethodName(Y_PASS_METHOD_USED_ARGS(Args));                                                       \
    });                                                                                                                  \
    return DoCall<ResultType>(Config_->ClusterRetryAttempts, callee);                                                   \
} Y_SEMICOLON_GUARD

CLIENT_METHOD_IMPL(TUnversionedLookupRowsResult, LookupRows, (const NYPath::TYPath&, NTableClient::TNameTablePtr, const TSharedRange<NTableClient::TLegacyKey>&, const TLookupRowsOptions&));
CLIENT_METHOD_IMPL(TSelectRowsResult, SelectRows, (const std::string&, const TSelectRowsOptions&));
CLIENT_METHOD_IMPL(std::vector<TUnversionedLookupRowsResult>, MultiLookupRows, (const std::vector<TMultiLookupSubrequest>&, const TMultiLookupOptions&));
CLIENT_METHOD_IMPL(TVersionedLookupRowsResult, VersionedLookupRows, (const NYPath::TYPath&, NTableClient::TNameTablePtr, const TSharedRange<NTableClient::TUnversionedRow>&, const TVersionedLookupRowsOptions&));
CLIENT_METHOD_IMPL(TPullRowsResult, PullRows, (const NYPath::TYPath&, const TPullRowsOptions&));
CLIENT_METHOD_IMPL(NQueueClient::IQueueRowsetPtr, PullQueue, (const NYPath::TRichYPath&, i64, int, const NQueueClient::TQueueRowBatchReadOptions&, const TPullQueueOptions&));
CLIENT_METHOD_IMPL(NQueueClient::IQueueRowsetPtr, PullQueueConsumer, (const NYPath::TRichYPath&, const NYPath::TRichYPath&, std::optional<i64>, int, const NQueueClient::TQueueRowBatchReadOptions&, const TPullQueueConsumerOptions&));
CLIENT_METHOD_IMPL(NYson::TYsonString, ExplainQuery, (const std::string&, const TExplainQueryOptions&));
CLIENT_METHOD_IMPL(NYson::TYsonString, GetNode, (const NYPath::TYPath&, const TGetNodeOptions&));
CLIENT_METHOD_IMPL(NYson::TYsonString, ListNode, (const NYPath::TYPath&, const TListNodeOptions&));
CLIENT_METHOD_IMPL(bool, NodeExists, (const NYPath::TYPath&, const TNodeExistsOptions&));
CLIENT_METHOD_IMPL(std::vector<TTabletInfo>, GetTabletInfos, (const NYPath::TYPath&, const std::vector<int>&, const TGetTabletInfosOptions&));
CLIENT_METHOD_IMPL(NChaosClient::TReplicationCardPtr, GetReplicationCard, (NChaosClient::TReplicationCardId, const TGetReplicationCardOptions&));
CLIENT_METHOD_IMPL(std::vector<TListQueueConsumerRegistrationsResult>, ListQueueConsumerRegistrations, (const std::optional<NYPath::TRichYPath>&, const std::optional<NYPath::TRichYPath>&, const TListQueueConsumerRegistrationsOptions&));

const NTabletClient::ITableMountCachePtr& TClient::GetTableMountCache()
{
    auto [client, _] = GetActiveClient();
    return client->GetTableMountCache();
}

const NTransactionClient::ITimestampProviderPtr& TClient::GetTimestampProvider()
{
    auto [client, _] = GetActiveClient();
    return client->GetTimestampProvider();
}

ITransactionPtr TClient::AttachTransaction(
    NTransactionClient::TTransactionId transactionId,
    const TTransactionAttachOptions& options)
{
    auto transactionClusterTag = NObjectClient::CellTagFromId(transactionId);
    for (const auto& clientDescription : UnderlyingClients_) {
        const auto& client = clientDescription->Client;
        auto clientClusterTag = client->GetConnection()->GetClusterTag();
        if (clientClusterTag == transactionClusterTag) {
            return client->AttachTransaction(transactionId, options);
        }
    }
    THROW_ERROR_EXCEPTION("No client is known for transaction %v", transactionId);
}

IPrerequisitePtr TClient::AttachPrerequisite(
    NPrerequisiteClient::TPrerequisiteId prerequisiteId,
    const TPrerequisiteAttachOptions& options)
{
    TTransactionAttachOptions attachOptions = {};
    attachOptions.AutoAbort = options.AutoAbort;
    attachOptions.PingPeriod = options.PingPeriod;
    attachOptions.Ping = options.Ping;
    attachOptions.PingAncestors = options.PingAncestors;

    static_assert(std::is_convertible_v<ITransaction*, IPrerequisite*>);

    return AttachTransaction(prerequisiteId, attachOptions);
}

void TClient::HandleError(const TErrorOr<void>& error, int clientIndex)
{
    if (!NRpc::IsChannelFailureError(error) && !Config_->RetryAnyError) {
        return;
    }

    UnderlyingClients_[clientIndex]->HasErrors = true;
    if (ActiveClientIndex_ != clientIndex) {
        return;
    }
    UpdateActiveClient();
}

void TClient::UpdateActiveClient()
{
    YT_ASSERT_THREAD_AFFINITY_ANY();

    for (int index = 0; index < std::ssize(UnderlyingClients_); ++index) {
        const auto& clientDescription = UnderlyingClients_[index];
        if (!clientDescription->HasErrors) {
            if (ActiveClientIndex_ != index) {
                YT_LOG_DEBUG("Active client was changed (PreviousClientIndex: %v, NewClientIndex: %v)",
                    ActiveClientIndex_.load(),
                    index);
                auto guard = NThreading::WriterGuard(Lock_);
                ActiveClientIndex_ = index;
                ActiveClient_ = clientDescription->Client;
            }
            return;
        }
    }
}

TClient::TActiveClientInfo TClient::GetActiveClient()
{
    YT_LOG_TRACE("Request will be send to the active client (ClientIndex: %v)",
        ActiveClientIndex_.load());
    auto guard = ReaderGuard(Lock_);
    return {ActiveClient_, ActiveClientIndex_.load()};
}

DEFINE_REFCOUNTED_TYPE(TClient)

////////////////////////////////////////////////////////////////////////////////

IClientPtr CreateClient(
    std::vector<NApi::IClientPtr> clients,
    TFederationConfigPtr config)
{
    return New<TClient>(
        std::move(clients),
        std::move(config));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClient::NFederated
