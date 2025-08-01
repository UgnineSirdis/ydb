#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <ydb/core/client/minikql_compile/mkql_compile_service.h>
#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/common/kqp.h>
#include <ydb/core/kqp/gateway/kqp_metadata_loader.h>
#include <ydb/core/kqp/host/kqp_host_impl.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/proto/accessor.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <yql/essentials/core/services/mounts/yql_mounts.h>
#include <yql/essentials/providers/common/provider/yql_provider.h>

#include <library/cpp/json/json_reader.h>

#include <util/string/printf.h>

#include <format>

namespace NKikimr {
namespace NKqp {

using namespace NYdb;
using namespace NYdb::NTable;
using namespace NYdb::NScripting;

using NYql::TExprContext;
using NYql::TExprNode;

namespace {

constexpr const char* TestCluster = "local_ut";

TIntrusivePtr<NKqp::IKqpGateway> GetIcGateway(Tests::TServer& server) {
    auto counters = MakeIntrusive<TKqpRequestCounters>();
    counters->Counters = new TKqpCounters(server.GetRuntime()->GetAppData(0).Counters);
    counters->TxProxyMon = new NTxProxy::TTxProxyMon(server.GetRuntime()->GetAppData(0).Counters);
    std::shared_ptr<NYql::IKikimrGateway::IKqpTableMetadataLoader> loader = std::make_shared<TKqpTableMetadataLoader>(TestCluster, server.GetRuntime()->GetAnyNodeActorSystem(),TIntrusivePtr<NYql::TKikimrConfiguration>(nullptr),false);
    return NKqp::CreateKikimrIcGateway(TestCluster, NKikimrKqp::QUERY_TYPE_SQL_GENERIC_QUERY, "/Root", "/Root", std::move(loader), server.GetRuntime()->GetAnyNodeActorSystem(),
        server.GetRuntime()->GetNodeId(0), counters, server.GetSettings().AppConfig->GetQueryServiceConfig());
}

TIntrusivePtr<IKqpHost> CreateKikimrQueryProcessor(TIntrusivePtr<IKqpGateway> gateway,
    const TString& cluster, NYql::IModuleResolver::TPtr moduleResolver,
    NActors::TActorSystem* actorSystem,
    const NKikimr::NMiniKQL::IFunctionRegistry* funcRegistry = nullptr,
    const TVector<NKikimrKqp::TKqpSetting>& settings = {}, bool keepConfigChanges = false)
{

    NYql::TKikimrConfiguration::TPtr kikimrConfig = MakeIntrusive<NYql::TKikimrConfiguration>();
    auto defaultSettingsData = NResource::Find("kqp_default_settings.txt");
    TStringInput defaultSettingsStream(defaultSettingsData);
    NKikimrKqp::TKqpDefaultSettings defaultSettings;
    UNIT_ASSERT(TryParseFromTextFormat(defaultSettingsStream, defaultSettings));
    kikimrConfig->Init(defaultSettings.GetDefaultSettings(), cluster, settings, true);

    auto federatedQuerySetup = std::make_optional<TKqpFederatedQuerySetup>({NYql::IHTTPGateway::Make(), nullptr, nullptr, nullptr, {}, {}, {}, nullptr, {}, nullptr, nullptr, {}, nullptr, {}, nullptr, nullptr, nullptr});
    return NKqp::CreateKqpHost(gateway, cluster, "/Root", kikimrConfig, moduleResolver,
                               federatedQuerySetup, nullptr, nullptr, NKikimrConfig::TQueryServiceConfig(), {}, funcRegistry, funcRegistry, keepConfigChanges, nullptr, actorSystem, nullptr);
}

NYql::NNodes::TExprBase GetExpr(const TString& ast, NYql::TExprContext& ctx, NYql::IModuleResolver* moduleResolver) {
    NYql::TAstParseResult astRes = NYql::ParseAst(ast);
    YQL_ENSURE(astRes.IsOk());
    NYql::TExprNode::TPtr result;
    YQL_ENSURE(CompileExpr(*astRes.Root, result, ctx, moduleResolver, nullptr));
    return NYql::NNodes::TExprBase(result);
}

void CreateTableWithIndexWithState(
    Tests::TServer& server,
    const TString& indexName,
    TIntrusivePtr<NKqp::IKqpGateway> gateway,
    NYql::TIndexDescription::EIndexState state)
{
    using NYql::TKikimrTableMetadataPtr;
    using NYql::TKikimrTableMetadata;
    using NYql::TKikimrColumnMetadata;

    TKikimrTableMetadataPtr metadata = new TKikimrTableMetadata(TestCluster, server.GetSettings().DomainName + "/IndexedTableWithState");
    metadata->Columns["key"] = TKikimrColumnMetadata("key", 0, "Uint32", false);
    metadata->Columns["value"] = TKikimrColumnMetadata("value", 0, "String", false);
    metadata->Columns["value2"] = TKikimrColumnMetadata("value2", 0, "String", false);
    metadata->ColumnOrder = {"key", "value", "value2"};
    metadata->Indexes.push_back(
        NYql::TIndexDescription(
            indexName,
            {"value"},
            TVector<TString>(),
            NYql::TIndexDescription::EType::GlobalSync,
            state,
            0,
            0,
            0,
            {}
        )
    );
    metadata->KeyColumnNames.push_back("key");

    {
        auto gatewayProxy = CreateKqpGatewayProxy(gateway, nullptr, server.GetRuntime()->GetAnyNodeActorSystem());
        auto result = gatewayProxy->CreateTable(metadata, true).ExtractValueSync();
        UNIT_ASSERT(result.Success());
    }
}

} // namespace


Y_UNIT_TEST_SUITE(KqpIndexMetadata) {
    Y_UNIT_TEST(HandleNotReadyIndex) {
        using namespace NYql;
        using namespace NYql::NNodes;

        auto setting = NKikimrKqp::TKqpSetting();

        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        

        auto& server = kikimr.GetTestServer();
        auto gateway = GetIcGateway(server);
        const TString& indexName = "value_index";

        CreateTableWithIndexWithState(server, indexName, gateway, NYql::TIndexDescription::EIndexState::NotReady);

        TExprContext moduleCtx;
        NYql::IModuleResolver::TPtr moduleResolver;
        YQL_ENSURE(GetYqlDefaultModuleResolver(moduleCtx, moduleResolver));
        auto qp = CreateKikimrQueryProcessor(gateway, TestCluster, moduleResolver,
            server.GetRuntime()->GetAnyNodeActorSystem(), server.GetFunctionRegistry());

        {
            const TString query = Q_(R"(
                UPSERT INTO `/Root/IndexedTableWithState` (key, value, value2)
                    VALUES (1, "qq", "ww")
            )");

            auto explainResult = qp->SyncExplainDataQuery(query, true);

            UNIT_ASSERT_C(explainResult.Success(), explainResult.Issues().ToString());

            TExprContext exprCtx;
            VisitExpr(GetExpr(explainResult.QueryAst, exprCtx, moduleResolver.get()).Ptr(),
                [&indexName](const TExprNode::TPtr& exprNode) {
                    if (TMaybeNode<TKqpUpsertRows>(exprNode)) {
                        UNIT_ASSERT(!TKqpUpsertRows(exprNode).Table().Path().Value().Contains(indexName));
                    }
                    if (TMaybeNode<TKqpDeleteRows>(exprNode)) {
                        UNIT_ASSERT(!TKqpDeleteRows(exprNode).Table().Path().Value().Contains(indexName));
                    }
                    return true;
                });
        }

        {
            const TString query = Q_("SELECT * FROM `/Root/IndexedTableWithState` VIEW value_index WHERE value = \"q\";");
            auto result = qp->SyncExplainDataQuery(query, true);
            UNIT_ASSERT(!result.Success());
            UNIT_ASSERT_C(result.Issues().ToString().Contains("No global indexes for table"), result.Issues().ToString());
        }
    }

    void TestNoReadFromMainTableBeforeJoin() {
        using namespace NYql;
        using namespace NYql::NNodes;

        TKikimrSettings settings;
        NKikimrConfig::TAppConfig appConfig;
        settings.SetAppConfig(appConfig);

        TKikimrRunner kikimr(settings);

        auto& server = kikimr.GetTestServer();
        auto gateway = GetIcGateway(server);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            const TString createTableSql(R"(
                --!syntax_v1
                CREATE TABLE `/Root/tg` (
                    id Utf8, b Utf8, am Decimal(22, 9), cur Utf8, pa_id Utf8, system_date Timestamp, status Utf8, type Utf8, product Utf8,
                    PRIMARY KEY (b, id),
                    INDEX tg_index GLOBAL SYNC ON (`b`, `pa_id`, `system_date`, `id`)
                    COVER(status, type, product, am)
                );)");
            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        // core optimizer should inject CoExtractMembert over KqlReadTableIndex with columns set based on ORDER BY
        // after that KqlReadTableIndex has all columns present in index and should be rewriten in to index read
        // limit must be applied in to this (index read) stage.
        // As result we have limited numbers of lookups from main table

        {
            const TString query(Q1_(R"(
                --!syntax_v1

                DECLARE $b_1 AS Text;
                DECLARE $pa_id_1 AS Text;
                DECLARE $b_2 AS Text;
                DECLARE $constant_param_1 AS Timestamp;
                DECLARE $constant_param_2 AS Text;
                DECLARE $type_1 AS List<Text>;
                DECLARE $status_1 AS Text;
                DECLARE $am_1 AS Decimal(22, 9);

                SELECT *
                FROM tg
                WHERE (`tg`.`b` = $b_1) AND (`tg`.`id` IN (
                    SELECT `id`
                    FROM (
                        SELECT *
                        FROM `/Root/tg` VIEW tg_index AS tg
                        WHERE (`tg`.`pa_id` = $pa_id_1)
                            AND (`tg`.`b` = $b_2)
                            AND ((`tg`.`system_date`, `tg`.`id`) <= ($constant_param_1, $constant_param_2))
                            AND (`tg`.`type` NOT IN $type_1)
                            AND (`tg`.`status` <> $status_1)
                            AND (`tg`.`am` <> $am_1)
                        ORDER BY
                            `tg`.`b` DESC,
                            `tg`.`pa_id` DESC,
                            `tg`.`system_date` DESC,
                            `tg`.`id` DESC
                        LIMIT 11)
                    ))
                ORDER BY
                    `tg`.`system_date` DESC,
                    `tg`.`id` DESC

            )"));

            auto explainResult = session.ExplainDataQuery(query).GetValueSync();
            UNIT_ASSERT_C(explainResult.IsSuccess(), explainResult.GetIssues().ToString());

            Cerr << explainResult.GetAst() << Endl;
            UNIT_ASSERT_C(explainResult.GetAst().contains("'('\"Reverse\")"), explainResult.GetAst());
            UNIT_ASSERT_C(explainResult.GetAst().contains("'('\"Sorted\")"), explainResult.GetAst());

            NJson::TJsonValue plan;
            NJson::ReadJsonTree(explainResult.GetPlan(), &plan, true);
            UNIT_ASSERT(ValidatePlanNodeIds(plan));
            Cerr << plan << Endl;
            auto mainTableAccess = CountPlanNodesByKv(plan, "Table", "tg");
            UNIT_ASSERT_VALUES_EQUAL(mainTableAccess, 1);

            auto indexTableAccess = CountPlanNodesByKv(plan, "Table", "tg/tg_index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(indexTableAccess, 1);

            auto filterOnIndex = CountPlanNodesByKv(plan, "Node Type", "Limit-Filter");
            UNIT_ASSERT_VALUES_EQUAL(filterOnIndex, 1);

            auto limitFilterNode = FindPlanNodeByKv(plan, "Node Type", "Limit-Filter");
            auto val = FindPlanNodes(limitFilterNode, "Limit");
            UNIT_ASSERT_VALUES_EQUAL(val.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(val[0], "11");
        }
    }

    Y_UNIT_TEST(TestNoReadFromMainTableBeforeJoin) {
        TestNoReadFromMainTableBeforeJoin();
    }

    Y_UNIT_TEST(HandleWriteOnlyIndex) {
        using namespace NYql;
        using namespace NYql::NNodes;

        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        

        auto& server = kikimr.GetTestServer();
        auto gateway = GetIcGateway(server);
        const TString& indexName = "value_index";

        CreateTableWithIndexWithState(server, indexName, gateway, NYql::TIndexDescription::EIndexState::WriteOnly);

        TExprContext moduleCtx;
        NYql::IModuleResolver::TPtr moduleResolver;
        YQL_ENSURE(GetYqlDefaultModuleResolver(moduleCtx, moduleResolver));
        auto qp = CreateKikimrQueryProcessor(gateway, TestCluster, moduleResolver,
            server.GetRuntime()->GetAnyNodeActorSystem(), server.GetFunctionRegistry());

        {
            const TString query = Q_(R"(
                UPSERT INTO `/Root/IndexedTableWithState` (key, value, value2)
                    VALUES (1, "qq", "ww");
            )");
            auto explainResult = qp->SyncExplainDataQuery(query, true);
            UNIT_ASSERT_C(explainResult.Success(), explainResult.Issues().ToString());

            TExprContext exprCtx;
            bool indexUpdated = false;
            bool indexCleaned = false;
            VisitExpr(GetExpr(explainResult.QueryAst, exprCtx, moduleResolver.get()).Ptr(),
                [&indexName, &indexUpdated, &indexCleaned](const TExprNode::TPtr& exprNode) mutable {
                    if (TMaybeNode<TKqpUpsertRows>(exprNode)) {
                        if (TKqpUpsertRows(exprNode).Table().Path().Value().Contains(indexName)) {
                            indexUpdated = true;
                        }
                    }
                    if (TMaybeNode<TKqpDeleteRows>(exprNode)) {
                        if (TKqpDeleteRows(exprNode).Table().Path().Value().Contains(indexName)) {
                            indexCleaned = true;
                        }
                    }
                    return true;
                });
            UNIT_ASSERT(indexUpdated);
            UNIT_ASSERT(indexCleaned);
        }

        {
            const TString query = Q_("SELECT * FROM `/Root/IndexedTableWithState` VIEW value_index WHERE value = \"q\";");
            auto result = qp->SyncExplainDataQuery(query, true);
            UNIT_ASSERT(!result.Success());
            UNIT_ASSERT(result.Issues().ToString().Contains("is not ready to use"));
        }
    }
}

Y_UNIT_TEST_SUITE(KqpIndexes) {
    Y_UNIT_TEST(NullInIndexTableNoDataRead) {
        auto setting = NKikimrKqp::TKqpSetting();
        TKikimrRunner kikimr({setting});
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);
        {
            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

            const TString query(Q1_(R"(
                SELECT Key FROM `/Root/SecondaryKeys` VIEW Index WHERE Fk IS NULL;
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                     execSettings)
                              .ExtractValueSync();
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[#];[[7]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);

            int phase = 0;
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(phase).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(phase).table_access(0).name(), "/Root/SecondaryKeys/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(phase).table_access(0).reads().rows(), 2);

        }
    }

    Y_UNIT_TEST(NullInIndexTable) {
        auto setting = NKikimrKqp::TKqpSetting();
        TKikimrRunner kikimr({setting});
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/SecondaryKeys` VIEW Index WHERE Fk IS NULL;
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[#;#;[\"Payload8\"]];[#;[7];[\"Payload7\"]]]");
        }
    }

    Y_UNIT_TEST(WriteWithParamsFieldOrder) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            const std::string keyColumnName = "key";
            auto builder = TTableBuilder()
               .AddNullableColumn(keyColumnName, EPrimitiveType::Uint64);

            builder.AddNullableColumn("index_0", EPrimitiveType::Utf8);
            builder.AddSecondaryIndex("index_0_name", std::vector<std::string>{"index_0", keyColumnName});
            builder.AddNullableColumn("value", EPrimitiveType::Uint32);
            builder.SetPrimaryKeyColumns({keyColumnName});

            auto result = session.CreateTable("/Root/Test1", builder.Build()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const TString query1(Q1_(R"(
                DECLARE $items AS List<Struct<'key':Uint64,'index_0':Utf8,'value':Uint32>>;
                UPSERT INTO `/Root/Test1`
                    SELECT * FROM AS_TABLE($items);
            )"));

            TParamsBuilder builder;
            builder.AddParam("$items").BeginList()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("key").Uint64(646464646464)
                        .AddMember("index_0").Utf8("SomeUtf8Data")
                        .AddMember("value").Uint32(323232)
                    .EndStruct()
                .EndList()
            .Build();

            static const TTxControl txControl = TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx();
            auto result = session.ExecuteDataQuery(query1, txControl, builder.Build()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/Test1");
            const TString expected = R"([[[646464646464u];["SomeUtf8Data"];[323232u]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/Test1/index_0_name/indexImplTable");
            const TString expected = R"([[["SomeUtf8Data"];[646464646464u]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST(SelectConcurentTX) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1", "Value1"),
                ("Primary2", "Secondary2", "Value2"),
                ("Primary3", "Secondary3", "Value3");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query1(Q_(R"(
            SELECT Index2 FROM `/Root/TestTable` WHERE Key = "Primary1";
        )"));

        // Start tx1, select from table by pk (without touching index table) and without commit
        auto result1 = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()))
                          .ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), NYdb::EStatus::SUCCESS);
        {
            auto yson = NYdb::FormatResultSetYson(result1.GetResultSet(0));
            UNIT_ASSERT_VALUES_EQUAL(yson, R"([[["Secondary1"]]])");
        }

        {
            // In other tx, update string
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1New", "Value1New")
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query2(Q1_(R"(
            SELECT Index2 FROM `/Root/TestTable` VIEW Index WHERE Index2 = "Secondary1";
        )"));

        UNIT_ASSERT(result1.GetTransaction());
        // Continue tx1, select from index table only
        auto result2 = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::Tx(result1.GetTransaction().value()).CommitTx())
                          .ExtractValueSync();
        // read only tx should succeed in MVCC case
        UNIT_ASSERT_VALUES_EQUAL_C(result2.GetStatus(), NYdb::EStatus::SUCCESS, result2.GetIssues().ToString());
    }

    Y_UNIT_TEST(SelectConcurentTX2) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1", "Value1"),
                ("Primary2", "Secondary2", "Value2"),
                ("Primary3", "Secondary3", "Value3");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query1 = Q1_(R"(
            SELECT Index2 FROM `/Root/TestTable` VIEW Index WHERE Index2 = "Secondary1";
        )");

        // Start tx1, select from table by index
        auto result1 = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()))
                          .ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), NYdb::EStatus::SUCCESS);
        {
            auto yson = NYdb::FormatResultSetYson(result1.GetResultSet(0));
            UNIT_ASSERT_VALUES_EQUAL(yson, R"([[["Secondary1"]]])");
        }

        {
            // In other tx, update string
            const TString query(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1New", "Value1New")
            )"));

            auto result = session.ExecuteDataQuery(
                                 query,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query2(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
            ("Primary4", "Secondary4", "Value4")
        )"));

        UNIT_ASSERT(result1.GetTransaction());
        // Continue tx1, write to table should fail in both MVCC and non-MVCC scenarios
        auto result2 = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::Tx(result1.GetTransaction().value()).CommitTx())
                          .ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result2.GetStatus(), NYdb::EStatus::ABORTED, result2.GetIssues().ToString().c_str());
    }

    Y_UNIT_TEST(UniqIndexComplexPkComplexFkOverlap) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("k1", EPrimitiveType::String)
                .AddNullableColumn("k2", EPrimitiveType::String)
                .AddNullableColumn("fk1", EPrimitiveType::String)
                .AddNullableColumn("fk2", EPrimitiveType::Int32)
                .AddNullableColumn("fk3", EPrimitiveType::Uint64)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"k1", "k2"});
            tableBuilder.AddUniqueSecondaryIndex("Index", std::vector<std::string>{"fk1", "fk2", "fk3", "k2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            // Upsert - add new rows
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (k1, k2, fk1, fk2, fk3, Value) VALUES
                ("p1str1", NULL, "fk1_str", 1000, 1000000000u, "Value1_1"),
                ("p1str2", NULL, "fk1_str", 1000, 1000000000u, "Value1_1");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["fk1_str"];[1000];[1000000000u];#;["p1str1"]];[["fk1_str"];[1000];[1000000000u];#;["p1str2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["p1str1"];#;["fk1_str"];[1000];[1000000000u];["Value1_1"]];[["p1str2"];#;["fk1_str"];[1000];[1000000000u];["Value1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            // Upsert - add new rows
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (k1, k2, fk1, fk2, fk3, Value) VALUES
                ("p1str3", "p2str3", "fk1_str", 1000, 1000000000u, "Value1_1");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["fk1_str"];[1000];[1000000000u];#;["p1str1"]];[["fk1_str"];[1000];[1000000000u];#;["p1str2"]];[["fk1_str"];[1000];[1000000000u];["p2str3"];["p1str3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["p1str1"];#;["fk1_str"];[1000];[1000000000u];["Value1_1"]];[["p1str2"];#;["fk1_str"];[1000];[1000000000u];["Value1_1"]];[["p1str3"];["p2str3"];["fk1_str"];[1000];[1000000000u];["Value1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            // Upsert - add new pk but broke uniq constraint for index
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (k1, k2, fk1, fk2, fk3, Value) VALUES
                ("p1str4", "p2str3", "fk1_str", 1000, 1000000000u, "Value1_1");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            // Upsert - add new rows with NULL in one of fk
            // set k2 to p2str3 to make conflict on the next itteration
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (k1, k2, fk1, fk2, fk3, Value) VALUES
                ("p1str5", "p2str3", NULL, 1000, 1000000000u, "Value1_1");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[#;[1000];[1000000000u];["p2str3"];["p1str5"]];[["fk1_str"];[1000];[1000000000u];#;["p1str1"]];[["fk1_str"];[1000];[1000000000u];#;["p1str2"]];[["fk1_str"];[1000];[1000000000u];["p2str3"];["p1str3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["p1str1"];#;["fk1_str"];[1000];[1000000000u];["Value1_1"]];[["p1str2"];#;["fk1_str"];[1000];[1000000000u];["Value1_1"]];[["p1str3"];["p2str3"];["fk1_str"];[1000];[1000000000u];["Value1_1"]];[["p1str5"];["p2str3"];#;[1000];[1000000000u];["Value1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            // Upsert - conflict with [["p1str1"];#;["fk1_str"];[1000];[1000000000u];["Value1_1"]] during update [["p1str5"];["p2str3"];#;[1000];[1000000000u];["Value1_1"]]
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (k1, k2, fk1) VALUES
                ("p1str5", "p2str3", "fk1_str");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[#;[1000];[1000000000u];["p2str3"];["p1str5"]];[["fk1_str"];[1000];[1000000000u];#;["p1str1"]];[["fk1_str"];[1000];[1000000000u];#;["p1str2"]];[["fk1_str"];[1000];[1000000000u];["p2str3"];["p1str3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["p1str1"];#;["fk1_str"];[1000];[1000000000u];["Value1_1"]];[["p1str2"];#;["fk1_str"];[1000];[1000000000u];["Value1_1"]];[["p1str3"];["p2str3"];["fk1_str"];[1000];[1000000000u];["Value1_1"]];[["p1str5"];["p2str3"];#;[1000];[1000000000u];["Value1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            // Insert - do nothing
            const TString query2 = Q1_(R"(
                INSERT INTO `/Root/TestTable` (k1, k2, fk1) VALUES
                ("p1str5", "p2str3", "fk1_str");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            const TString query2 = Q1_(R"(
                UPDATE `/Root/TestTable` ON (k1, k2, fk1) VALUES
                ("p1str5", "p2str3", "fk1_str");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            const TString query2 = Q1_(R"(
                UPDATE `/Root/TestTable` SET fk1 = "fk1_str"
                WHERE k1 = "p1str5" AND k2 = "p2str3";
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(UpsertMultipleUniqIndexes) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("k1", EPrimitiveType::String)
                .AddNullableColumn("fk1", EPrimitiveType::String)
                .AddNullableColumn("fk2", EPrimitiveType::Int32)
                .AddNullableColumn("fk3", EPrimitiveType::Uint64)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"k1"});
            tableBuilder.AddUniqueSecondaryIndex("Index12", std::vector<std::string>{"fk1", "fk2"}, std::vector<std::string>{"Value"});
            tableBuilder.AddUniqueSecondaryIndex("Index3", std::vector<std::string>{"fk3"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            // Upsert - add new rows
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (k1, fk1, fk2, fk3, Value) VALUES
                ("p1str1", "fk1_str1", 1000, 1000000000u, "Value1_1"),
                ("p1str2", "fk1_str2", 1000, 1000000001u, "Value1_1");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index12/indexImplTable");
            const TString expected = R"([[["fk1_str1"];[1000];["p1str1"];["Value1_1"]];[["fk1_str2"];[1000];["p1str2"];["Value1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index3/indexImplTable");
            const TString expected = R"([[[1000000000u];["p1str1"]];[[1000000001u];["p1str2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["p1str1"];["fk1_str1"];[1000];[1000000000u];["Value1_1"]];[["p1str2"];["fk1_str2"];[1000];[1000000001u];["Value1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (k1, fk3, Value) VALUES
                ("p1str2", 1000000000u, "Value1_2");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index12/indexImplTable");
            const TString expected = R"([[["fk1_str1"];[1000];["p1str1"];["Value1_1"]];[["fk1_str2"];[1000];["p1str2"];["Value1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index3/indexImplTable");
            const TString expected = R"([[[1000000000u];["p1str1"]];[[1000000001u];["p1str2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["p1str1"];["fk1_str1"];[1000];[1000000000u];["Value1_1"]];[["p1str2"];["fk1_str2"];[1000];[1000000001u];["Value1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (k1, fk3, Value) VALUES
                ("p1str2", 2000000000u, "Value1_3");
            )");

            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index12/indexImplTable");
            const TString expected = R"([[["fk1_str1"];[1000];["p1str1"];["Value1_1"]];[["fk1_str2"];[1000];["p1str2"];["Value1_3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index3/indexImplTable");
            const TString expected = R"([[[1000000000u];["p1str1"]];[[2000000000u];["p1str2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["p1str1"];["fk1_str1"];[1000];[1000000000u];["Value1_1"]];[["p1str2"];["fk1_str2"];[1000];[2000000000u];["Value1_3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    void DoUpsertWithoutIndexUpdate(bool uniq, bool useSink) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableOltpSink(useSink);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(appConfig);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("fk1", EPrimitiveType::String)
                .AddNullableColumn("fk2", EPrimitiveType::Int32)
                .AddNullableColumn("fk3", EPrimitiveType::Uint64)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            if (uniq) {
                tableBuilder.AddUniqueSecondaryIndex("Index", std::vector<std::string>{"fk1", "fk2", "fk3"});
            } else {
                tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"fk1", "fk2", "fk3"});
            }
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        auto uniqExtraStages = uniq ? 5 : 0;
        {
            // Upsert - add new row
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (Key, fk1, fk3, Value) VALUES
                ("Primary1", "fk1_str", 1000000000u, "Value1_1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Profile);
            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            Cerr << stats.DebugString() << Endl;

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(),  uniqExtraStages + (useSink ? 4 : 5));

            // One read from main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniq + 1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniq + 1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniq + 1).table_access(0).reads().rows(), 0);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + 2).table_access().size(), 0);

            if (!useSink) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + 3).table_access().size(), 0);
            }

            const auto finalStage = useSink ? 3 : 4;

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + finalStage).table_access().size(), 2);

            // One update of main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + finalStage).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + finalStage).table_access(0).updates().rows(), 1);
            UNIT_ASSERT(            !stats.query_phases(uniqExtraStages + finalStage).table_access(0).has_deletes());

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + finalStage).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + finalStage).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(            !stats.query_phases(uniqExtraStages + finalStage).table_access(1).has_deletes());

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["fk1_str"];#;[1000000000u];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            // Same query - should not touch index
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (Key, fk1, fk3, Value) VALUES
                ("Primary1", "fk1_str", 1000000000u, "Value1_1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Profile);
            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), uniqExtraStages + (useSink ? 4 : 5));

            // One read from main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniq + 1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniq + 1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniq + 1).table_access(0).reads().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + 2).table_access().size(), 0);

            if (!useSink) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + 3).table_access().size(), 0);
            }

            const auto finalStage = useSink ? 3 : 4;

            // One update of main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + finalStage).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + finalStage).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(uniqExtraStages + finalStage).table_access(0).updates().rows(), 1);
            UNIT_ASSERT(            !stats.query_phases(uniqExtraStages + finalStage).table_access(0).has_deletes());

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["fk1_str"];#;[1000000000u];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }
    }

    Y_UNIT_TEST_QUAD(DoUpsertWithoutIndexUpdate, UniqIndex, UseSink) {
        DoUpsertWithoutIndexUpdate(UniqIndex, UseSink);
    }

    Y_UNIT_TEST_TWIN(UpsertWithoutExtraNullDelete, UseSink) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(appConfig);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1", "Value1");
            )"));

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Profile);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseSink ? 4 : 5);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 0);

            if (!UseSink) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access().size(), 0);
            }

            const auto finalStage = UseSink ? 3 : 4;
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(0).updates().rows(), 1);
            UNIT_ASSERT(            !stats.query_phases(finalStage).table_access(0).has_deletes());

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(            !stats.query_phases(finalStage).table_access(1).has_deletes());
        }

        {
            const TString query1 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1_1", "Value1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Profile);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseSink ? 4 : 5);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 0);
            if (!UseSink) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access().size(), 0);
            }

            const auto finalStage = UseSink ? 3 : 4;

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(0).updates().rows(), 1);
            UNIT_ASSERT(            !stats.query_phases(finalStage).table_access(0).has_deletes());


            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(             stats.query_phases(finalStage).table_access(1).has_deletes());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(1).deletes().rows(), 1);
        }

        {
            // Upsert without touching index
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Value) VALUES
                ("Primary1", "Value1_1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Profile);
            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseSink ? 4 : 5);

            // One read from main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 0);

            const auto finalStage = UseSink ? 3 : 4;
            if (!UseSink) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access().size(), 0);
            }
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access().size(), 1);

            // One update of main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(finalStage).table_access(0).updates().rows(), 1);
            UNIT_ASSERT(            !stats.query_phases(finalStage).table_access(0).has_deletes());

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            // Update without touching index
            const TString query2 = Q1_(R"(
                UPDATE `/Root/TestTable` ON (Key, Value) VALUES
                ("Primary1", "Value1_2");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Profile);
            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseSink ? 1 : 4);

            int idx = UseSink ? 0 : 1;

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access().size(), 1);
            // One read of main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).reads().rows(), UseSink ? 0 : 1);

            
            // One update of index table
            idx += UseSink ? 0 : 2;
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).updates().rows(), 1);    

            // Thats it, no phase for index table - we remove it on compile time
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            // Update without touching index
            const TString query2 = Q1_(R"(
                UPDATE `/Root/TestTable` SET Value = "Value1_3"
                WHERE Key = "Primary1";
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Profile);
            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseSink ? 1 : 2);

            if (!UseSink) {
                // One read of main table
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 1);

                // One update of main table
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).updates().rows(), 1);
            } else {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).updates().rows(), 1);
            }

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }
    }

    Y_UNIT_TEST(UpsertWithNullKeysSimple) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({ setting });
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("IndexColumn", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, IndexColumn, Value) VALUES
                ("Primary 1", "Secondary 1", "Value 1"),
                (Null,        "Secondary 2", "Value 2");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            TAlterTableSettings alterSettings;
            alterSettings.AppendAddIndexes({ TIndexDescription("IndexName", {"IndexColumn"}) });
            auto result = session.AlterTable("/Root/TestTable", alterSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName WHERE IndexColumn = 'Secondary 2';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 2\"]]]");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName/indexImplTable");
            const TString expected =
                R"([[["Secondary 1"];["Primary 1"]];)"
                R"([["Secondary 2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, IndexColumn, Value) VALUES
                ("Primary 3", "Secondary 3", "Value 3"),
                (Null,        "Secondary 4", "Value 4");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName WHERE IndexColumn = 'Secondary 4';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 4\"]]]");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName/indexImplTable");
            const TString expected =
                R"([[["Secondary 1"];["Primary 1"]];)"
                R"([["Secondary 3"];["Primary 3"]];)"
                R"([["Secondary 4"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

    }

    Y_UNIT_TEST(UpsertWithNullKeysComplex) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            // Create table with 1 index
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("IndexColumn1", EPrimitiveType::String)
                .AddNullableColumn("IndexColumn2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key", "IndexColumn1", "IndexColumn2"});
            tableBuilder.AddSecondaryIndex("IndexName1", std::vector<std::string>{"IndexColumn1"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            // Upsert rows including one with PK starting with Null
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, IndexColumn1, IndexColumn2, Value) VALUES
                ("Primary 1", "Secondary1 1", "Secondary2 1", "Value 1"),
                (Null,       "Secondary1 2", "Secondary2 2", "Value 2");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            // Read row with PK starting with Null by index1
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName1 WHERE IndexColumn1 = 'Secondary1 2';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 2\"]]]");
        }
        {
            // Both rows should be in index1 table
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName1/indexImplTable");
            const TString expected =
                R"([[["Secondary1 1"];["Primary 1"];["Secondary2 1"]];)"
                R"([["Secondary1 2"];#;["Secondary2 2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            // Add second index with alter
            TAlterTableSettings alterSettings;
            alterSettings.AppendAddIndexes({ TIndexDescription("IndexName2", {"IndexColumn2"}) });
            auto result = session.AlterTable("/Root/TestTable", alterSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            // Read row with PK starting with Null by index2
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName2 WHERE IndexColumn2 = 'Secondary2 2';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 2\"]]]");
        }
        {
            // Both rows should also be in index2 table
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName2/indexImplTable");
            const TString expected =
                R"([[["Secondary2 1"];["Primary 1"];["Secondary1 1"]];)"
                R"([["Secondary2 2"];#;["Secondary1 2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            // Upsert more rows including one with PK starting with Null
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, IndexColumn1, IndexColumn2, Value) VALUES
                ("Primary 3", "Secondary1 3", "Secondary2 3", "Value 3"),
                (Null,       "Secondary1 4", "Secondary2 4", "Value 4");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            // Read recently added row with PK starting with Null by index2
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName2 WHERE IndexColumn2 = 'Secondary2 4';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 4\"]]]");
        }
        {
            // All 4 rows should be in index1 table
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName1/indexImplTable");
            const TString expected =
                R"([[["Secondary1 1"];["Primary 1"];["Secondary2 1"]];)"
                R"([["Secondary1 2"];#;["Secondary2 2"]];)"
                R"([["Secondary1 3"];["Primary 3"];["Secondary2 3"]];)"
                R"([["Secondary1 4"];#;["Secondary2 4"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            // All 4 rows should also be in index2 table
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName2/indexImplTable");
            const TString expected =
                R"([[["Secondary2 1"];["Primary 1"];["Secondary1 1"]];)"
                R"([["Secondary2 2"];#;["Secondary1 2"]];)"
                R"([["Secondary2 3"];["Primary 3"];["Secondary1 3"]];)"
                R"([["Secondary2 4"];#;["Secondary1 4"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

    }

    Y_UNIT_TEST(SecondaryIndexUpsert1DeleteUpdate) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "SomeOldIndex", "SomeOldValue");
            )"));

            auto result = session.ExecuteDataQuery(
                                     query1,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
            ("Primary1", "Secondary1", "Value1"),
            ("Primary2", "Secondary2", "Value2"),
            ("Primary3", "Secondary3", "Value3");
        )"));

        auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1"];["Primary1"]];[["Secondary2"];["Primary2"]];[["Secondary3"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            auto result = session.DescribeTable("/Root/TestTable/Index/indexImplTable").ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::SUCCESS);

            const THashMap<std::string_view, std::string_view> columnTypes = {
                {"Key", "String?"},
                {"Index2", "String?"}
            };

            const auto& columns = result.GetTableDescription().GetTableColumns();
            UNIT_ASSERT_VALUES_EQUAL(columns.size(), columnTypes.size());
            for (const auto& column : columns) {
                UNIT_ASSERT_VALUES_EQUAL_C(column.Type.ToString(), columnTypes.at(column.Name), column.Name);
            }
        }

        {
            const TString query(Q_(R"(
                SELECT * FROM `/Root/TestTable/Index/indexImplTable`;
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::SUCCESS);
        }

        {
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW WrongView WHERE Index2 = 'Secondary2';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::SCHEME_ERROR);
        }

        {
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW Index WHERE Index2 = 'Secondary2';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value2\"]]]");
        }

        {
            const TString query(Q_(R"(
                DELETE FROM `/Root/TestTable` ON (Key) VALUES ('Primary1');
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary2"];["Primary2"]];[["Secondary3"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                DELETE FROM `/Root/TestTable` WHERE Key = 'Primary2';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary3"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, Index2) VALUES ('Primary3', 'Secondary3_1');
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary3"];["Secondary3_1"];["Value3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary3_1"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                UPDATE `/Root/TestTable` SET Index2 = 'Secondary3_2' WHERE Key = 'Primary3';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary3"];["Secondary3_2"];["Value3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary3_2"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST(SecondaryIndexUpsert2Update) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Index2A", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2", "Index2A"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Index2, Index2A) VALUES
            ("Primary1", "Secondary1", "Secondary1A"),
            ("Primary2", "Secondary2", "Secondary2A"),
            ("Primary3", "Secondary3", "Secondary3A");
        )"));

        auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1"];["Secondary1A"];["Primary1"]];[["Secondary2"];["Secondary2A"];["Primary2"]];[["Secondary3"];["Secondary3A"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, Index2) VALUES ('Primary1', 'Secondary1_1');
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Secondary1_1"];["Secondary1A"]];[["Primary2"];["Secondary2"];["Secondary2A"]];[["Primary3"];["Secondary3"];["Secondary3A"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1_1"];["Secondary1A"];["Primary1"]];[["Secondary2"];["Secondary2A"];["Primary2"]];[["Secondary3"];["Secondary3A"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                UPDATE `/Root/TestTable` SET Index2 = 'Secondary1_2' WHERE Key = 'Primary1';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Secondary1_2"];["Secondary1A"]];[["Primary2"];["Secondary2"];["Secondary2A"]];[["Primary3"];["Secondary3"];["Secondary3A"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1_2"];["Secondary1A"];["Primary1"]];[["Secondary2"];["Secondary2A"];["Primary2"]];[["Secondary3"];["Secondary3A"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST(SecondaryIndexUpdateOnUsingIndex) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", "Index2");
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
            ("Primary1", "Secondary1", "Val1");
        )"));

        auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1"];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {

            const TString query(Q1_(R"(
                UPDATE `/Root/TestTable` ON (Key, Index2, Value)
                    (SELECT Key, Index2, 'Val1_1' as Value FROM `/Root/TestTable` VIEW Index WHERE Index2 = 'Secondary1');
            )"));

            auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Secondary1"];["Val1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1"];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST(SecondaryIndexSelectUsingScripting) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        TScriptingClient client(kikimr.GetDriver());
        {
            auto session = db.CreateSession().GetValueSync().GetSession();
            const TString createTableSql(R"(
                --!syntax_v1
                CREATE TABLE `/Root/SharedHouseholds` (
                    guest_huid Uint64, guest_id Uint64, owner_huid Uint64, owner_id Uint64, household_id String,
                    PRIMARY KEY (guest_huid, owner_huid, household_id),
                    INDEX shared_households_owner_huid GLOBAL SYNC ON (`owner_huid`)
                );)");
            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const TString query(Q1_(R"(
                SELECT
                    guest_id
                FROM
                    SharedHouseholds VIEW shared_households_owner_huid
                WHERE
                    owner_huid == 1 AND
                    household_id == "1";
            )"));

            auto result = client.ExecuteYqlScript(query).GetValueSync();

            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[]");
        }
    }

    Y_UNIT_TEST(SecondaryIndexOrderBy) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});

        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::Int64)
                .AddNullableColumn("Index2", EPrimitiveType::Int64)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                (1, 1001, "Value1"),
                (2, 1002, "Value2"),
                (3, 1003, "Value3");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2 DESC;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());

                UNIT_ASSERT_C(result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString().c_str());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1003];[3];[\"Value3\"]];[[1002];[2];[\"Value2\"]];[[1001];[1];[\"Value1\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2 DESC LIMIT 2;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                UNIT_ASSERT_C(result.GetAst().contains("('\"ItemsLimit\""), result.GetAst());
                UNIT_ASSERT_C(result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1003];[3];[\"Value3\"]];[[1002];[2];[\"Value2\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT Index2, Key FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2 DESC LIMIT 2;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                UNIT_ASSERT_C(result.GetAst().contains("('\"ItemsLimit\""), result.GetAst());
                UNIT_ASSERT_C(result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1003];[3]];[[1002];[2]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t WHERE t.Index2 < 1003 ORDER BY t.Index2 DESC;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                UNIT_ASSERT_C(result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2];[\"Value2\"]];[[1001];[1];[\"Value1\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
                UNIT_ASSERT_C(!result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1001];[1];[\"Value1\"]];[[1002];[2];[\"Value2\"]];[[1003];[3];[\"Value3\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
                UNIT_ASSERT_C(!result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
                UNIT_ASSERT_C(result.GetAst().contains("('\"ItemsLimit\""), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1001];[1];[\"Value1\"]];[[1002];[2];[\"Value2\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t
                WHERE t.Index2 > 1001
                ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
                UNIT_ASSERT_C(result.GetAst().contains("('\"ItemsLimit\""), result.GetAst());
                UNIT_ASSERT_C(!result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2];[\"Value2\"]];[[1003];[3];[\"Value3\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t
                WHERE t.Index2 = 1002
                ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
                UNIT_ASSERT_C(result.GetAst().contains("('\"ItemsLimit\""), result.GetAst());
                UNIT_ASSERT_C(!result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2];[\"Value2\"]]]");
            }
        }


        {
            const TString query(Q1_(R"(
                SELECT Index2, Key FROM `/Root/TestTable` VIEW Index as t
                WHERE t.Index2 > 1001
                ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
                UNIT_ASSERT_C(result.GetAst().contains("('\"ItemsLimit\""), result.GetAst());
                UNIT_ASSERT_C(!result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2]];[[1003];[3]]]");
            }
        }

        {
            // Request by Key but using Index table, expect correct result
            const TString query(Q1_(R"(
                SELECT Index2, Key FROM `/Root/TestTable` VIEW Index as t
                WHERE t.Key > 1
                ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
                UNIT_ASSERT_C(!result.GetAst().contains("('\"ItemsLimit\""), result.GetAst());
                UNIT_ASSERT_C(!result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2]];[[1003];[3]]]");
            }
        }
    }

    Y_UNIT_TEST(ExplainCollectFullDiagnostics) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});

        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::Int64)
                .AddNullableColumn("Index2", EPrimitiveType::Int64)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2 DESC;
            )"));

            {
                auto settings = TExplainDataQuerySettings();
                settings.WithCollectFullDiagnostics(true);

                auto result = session.ExplainDataQuery(
                    query, settings)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());

                UNIT_ASSERT_C(result.GetAst().contains("'('\"Reverse\")"), result.GetAst());

                UNIT_ASSERT_C(!result.GetDiagnostics().empty(), "Query result diagnostics is empty");

                TStringStream in;
                in << result.GetDiagnostics();
                NJson::TJsonValue value;
                ReadJsonTree(&in, &value);

                UNIT_ASSERT_C(value.IsMap(), "Incorrect Diagnostics");
                UNIT_ASSERT_C(value.Has("query_id"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(value.Has("version"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(!value.Has("query_text"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(value.Has("query_parameter_types"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(value.Has("table_metadata"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(value["table_metadata"].IsArray(), "Incorrect Diagnostics: table_metadata type should be an array");
                UNIT_ASSERT_C(value.Has("created_at"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(value.Has("query_syntax"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(value.Has("query_database"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(value.Has("query_cluster"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(!value.Has("query_plan"), "Incorrect Diagnostics");
                UNIT_ASSERT_C(value.Has("query_type"), "Incorrect Diagnostics");
            }

            {
                auto settings = TExplainDataQuerySettings();

                auto result = session.ExplainDataQuery(
                    query, settings)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());

                UNIT_ASSERT_C(result.GetAst().contains("'('\"Reverse\")"), result.GetAst());

                UNIT_ASSERT_C(result.GetDiagnostics().empty(), "Query result diagnostics should be empty, but it's not");
            }
        }
    }

    Y_UNIT_TEST(SecondaryIndexOrderBy2) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("id", EPrimitiveType::Uint64)
                .AddNullableColumn("customer", EPrimitiveType::Utf8)
                .AddNullableColumn("created", EPrimitiveType::Datetime)
                .AddNullableColumn("processed", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"id"});
            tableBuilder.AddSecondaryIndex("ix_cust", std::vector<std::string>{"customer"});
            tableBuilder.AddSecondaryIndex("ix_cust2", std::vector<std::string>{"customer", "created"});
            tableBuilder.AddSecondaryIndex("ix_cust3", std::vector<std::string>{"customer", "created"}, std::vector<std::string>{"processed"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (id, customer, created, processed) VALUES
                (1, "Vasya", CAST('2020-01-01T00:00:01Z' as DATETIME), "Value1"),
                (2, "Vova",  CAST('2020-01-01T00:00:02Z' as DATETIME), "Value2"),
                (3, "Petya", CAST('2020-01-01T00:00:03Z' as DATETIME), "Value3"),
                (4, "Vasya", CAST('2020-01-01T00:00:04Z' as DATETIME), "Value4"),
                (5, "Vasya", CAST('2020-01-01T00:00:05Z' as DATETIME), "Value5");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW ix_cust as t WHERE t.customer = "Vasya"
                ORDER BY t.customer DESC, t.id DESC;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());
                UNIT_ASSERT_C(result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), execSettings)
                    .ExtractValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString().c_str());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1577836805u];[\"Vasya\"];[5u];[\"Value5\"]];[[1577836804u];[\"Vasya\"];[4u];[\"Value4\"]];[[1577836801u];[\"Vasya\"];[1u];[\"Value1\"]]]");

                auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(1).name(), "/Root/TestTable/ix_cust/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(1).reads().rows(), 3);
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW ix_cust2 as t WHERE t.customer = "Vasya" ORDER BY t.customer DESC, t.created DESC LIMIT 2;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());

                UNIT_ASSERT_C(result.GetAst().contains("'('\"ItemsLimit"), result.GetAst());
                UNIT_ASSERT_C(result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), execSettings)
                    .ExtractValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString().c_str());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1577836805u];[\"Vasya\"];[5u];[\"Value5\"]];[[1577836804u];[\"Vasya\"];[4u];[\"Value4\"]]]");

                auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 2);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(1).name(), "/Root/TestTable/ix_cust2/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(1).reads().rows(), 2);
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW ix_cust3 as t WHERE t.customer = "Vasya" ORDER BY t.customer DESC, t.created DESC LIMIT 2;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());
                UNIT_ASSERT_C(result.GetAst().contains("'('\"ItemsLimit"), result.GetAst());
                UNIT_ASSERT_C(result.GetAst().contains("'('\"Reverse\")"), result.GetAst());
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), execSettings)
                    .ExtractValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString().c_str());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1577836805u];[\"Vasya\"];[5u];[\"Value5\"]];[[1577836804u];[\"Vasya\"];[4u];[\"Value4\"]]]");

                auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

                int indexPhaseId = 0;
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable/ix_cust3/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
            }
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexReplace, UseSink) {
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(app);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("KeyA", EPrimitiveType::Uint64)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Index2A", EPrimitiveType::Uint8)
                .AddNullableColumn("Value", EPrimitiveType::Utf8);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key", "KeyA"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2", "Index2A"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, KeyA, Index2, Index2A, Value) VALUES
                    ("Primary1", 41, "Secondary1", 1, "Value1"),
                    ("Primary2", 42, "Secondary2", 2, "Value2"),
                    ("Primary3", 43, "Secondary3", 3, "Value3");
                )"));
            auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected =
R"([[["Primary1"];[41u];["Secondary1"];[1u];["Value1"]];[["Primary2"];[42u];["Secondary2"];[2u];["Value2"]];[["Primary3"];[43u];["Secondary3"];[3u];["Value3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected =
R"([[["Secondary1"];[1u];["Primary1"];[41u]];[["Secondary2"];[2u];["Primary2"];[42u]];[["Secondary3"];[3u];["Primary3"];[43u]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, KeyA, Value) VALUES
                    ("Primary1", 41, "Value1_1");
                )"));
            auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected =
R"([[["Primary1"];[41u];#;#;["Value1_1"]];[["Primary2"];[42u];["Secondary2"];[2u];["Value2"]];[["Primary3"];[43u];["Secondary3"];[3u];["Value3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected =
R"([[#;#;["Primary1"];[41u]];[["Secondary2"];[2u];["Primary2"];[42u]];[["Secondary3"];[3u];["Primary3"];[43u]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST(SecondaryIndexInsert1) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetWithSampleTables(false);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, Value) VALUES
                ("Primary1", "Value1"),
                ("Primary2", "Value2"),
                ("Primary3", "Value3");
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            result.GetIssues().PrintTo(Cerr);
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[#;["Primary1"]];[#;["Primary2"]];[#;["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(MultipleSecondaryIndex, UseSink) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(appConfig);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value1", EPrimitiveType::String)
                .AddNullableColumn("Value2", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value1");
            tableBuilder.AddSecondaryIndex("Index2", "Value2");
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
            ("Primary1", "Val1", "Val2");
        )"));

        auto explainResult = session.ExplainDataQuery(query1).ExtractValueSync();
        UNIT_ASSERT_C(explainResult.IsSuccess(), explainResult.GetIssues().ToString());

        NJson::TJsonValue plan;
        NJson::ReadJsonTree(explainResult.GetPlan(), &plan, true);

        UNIT_ASSERT(plan.GetMapSafe().contains("tables"));
        const auto& tables = plan.GetMapSafe().at("tables").GetArraySafe();
        UNIT_ASSERT(tables.size() == 3);
        UNIT_ASSERT(tables.at(0).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable");
        UNIT_ASSERT(tables.at(1).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable/Index1/indexImplTable");
        UNIT_ASSERT(tables.at(2).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable/Index2/indexImplTable");

        auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1"];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2"];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1"];["Val2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST(UniqAndNoUniqSecondaryIndex) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value1", EPrimitiveType::String)
                .AddNullableColumn("Value2", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddUniqueSecondaryIndex("Index1Uniq", {"Value1"});
            tableBuilder.AddSecondaryIndex("Index2NotUniq", "Value2");
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                ("Primary1", "Val1", "Val2");
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
       }

       {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                    ("Primary2", "Val1", "blabla");
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                    ("Primary3", "Val1", "blabla");
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                    ("Primary3", "Val1", "blabla");
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1Uniq/indexImplTable");
                const TString expected = R"([[["Val1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2NotUniq/indexImplTable");
                const TString expected = R"([[["Val2"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
                const TString expected = R"([[["Primary1"];["Val1"];["Val2"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` SET Value2 = "Val2_1" WHERE Key = "Primary1";
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1Uniq/indexImplTable");
                const TString expected = R"([[["Val1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2NotUniq/indexImplTable");
                const TString expected = R"([[["Val2_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
                const TString expected = R"([[["Primary1"];["Val1"];["Val2_1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }
    }

    void CheckUpsertNonEquatableType(bool notNull) {
        auto kqpSetting = NKikimrKqp::TKqpSetting();
        kqpSetting.SetName("_KqpYqlSyntaxVersion");
        kqpSetting.SetValue("1");

        auto settings = TKikimrSettings()
                .SetKqpSettings({kqpSetting});
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        TString createTableSql = R"(
            CREATE TABLE `TableWithJson` (
                id Int64,
                name Utf8,
                slug Json %s,
                parent_id Int64,
                PRIMARY KEY (id),
                INDEX json_parent_id_index GLOBAL ON (parent_id, id) COVER (name, slug)
            );
        )";

        createTableSql = Sprintf(createTableSql.data(), notNull ? "NOT NULL" : "");

        {
            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        const TString query = R"(
            UPSERT INTO `TableWithJson` (
                id,
                name,
                slug,
                parent_id
            )
            VALUES (
                1,
                'Q',
                JSON(@@"dispenser"@@),
                666
            );
        )";

        auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TableWithJson");
            const TString expected = R"([[[1];["Q"];["\"dispenser\""];[666]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TableWithJson/json_parent_id_index/indexImplTable");
            const TString expected = R"([[[666];[1];["Q"];["\"dispenser\""]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(CheckUpsertNonEquatableType, NotNull) {
        CheckUpsertNonEquatableType(NotNull);
    }

    Y_UNIT_TEST(UniqAndNoUniqSecondaryIndexWithCover) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value1", EPrimitiveType::String)
                .AddNullableColumn("Value2", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddUniqueSecondaryIndex("Index1Uniq", {"Value1"}, {"Value2"});
            tableBuilder.AddSecondaryIndex("Index2NotUniq", {"Value2"}, {"Value1"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                ("Primary1", "Val1", "Val2");
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
       }

       {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                    ("Primary2", "Val1", "blabla");
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                    ("Primary3", "Val1", "blabla");
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                    ("Primary3", "Val1", "blabla");
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        }

        {

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1Uniq/indexImplTable");
                const TString expected = R"([[["Val1"];["Primary1"];["Val2"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2NotUniq/indexImplTable");
                const TString expected = R"([[["Val2"];["Primary1"];["Val1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
                const TString expected = R"([[["Primary1"];["Val1"];["Val2"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` SET Value2 = "Val2_1" WHERE Key = "Primary1";
            )"));

            auto result = session.ExecuteDataQuery(
                    query1,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1Uniq/indexImplTable");
                const TString expected = R"([[["Val1"];["Primary1"];["Val2_1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2NotUniq/indexImplTable");
                const TString expected = R"([[["Val2_1"];["Primary1"];["Val1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
                const TString expected = R"([[["Primary1"];["Val1"];["Val2_1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }
    }


    Y_UNIT_TEST_TWIN(MultipleSecondaryIndexWithSameComulns, UseSink) {
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(app);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value1", EPrimitiveType::String)
                .AddNullableColumn("Value2", EPrimitiveType::String)
                .AddNullableColumn("Value3", EPrimitiveType::Int64);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", std::vector<std::string>{"Value1", "Value3"});
            tableBuilder.AddSecondaryIndex("Index2", std::vector<std::string>{"Value2", "Value3"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Value1, Value2, Value3) VALUES
                ("Primary1", "Val1", "Val2", 42);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1"];["Val2"];[42]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q1_(R"(
                SELECT Value2 FROM `/Root/TestTable` VIEW Index1 WHERE Value1 = 'Val1';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Val2\"]]]");
        }

        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, Value1, Value2, Value3) VALUES
                ("Primary1", "Val1_1", "Val2_1", 43);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];[43];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[43];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];["Val2_1"];[43]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                ("Primary1", "Val1_1", "Val2_1");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];#;["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];#;["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];["Val2_1"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` SET Value3 = 35;
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];[35];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[35];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];["Val2_1"];[35]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, Value3) VALUES ('Primary1', 36);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];["Val2_1"];[36]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, Value1) VALUES ('Primary1', 'Val1_2');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, Value2) VALUES ('Primary2', 'Record2');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[#;#;["Primary2"]];[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Record2"];#;["Primary2"]];[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]];[["Primary2"];#;["Record2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, Value3) VALUES ('Primary3', 37);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[#;#;["Primary2"]];[#;[37];["Primary3"]];[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[#;[37];["Primary3"]];[["Record2"];#;["Primary2"]];[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]];[["Primary2"];#;["Record2"];#];[["Primary3"];#;#;[37]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable` WHERE Key = 'Primary3';
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[#;#;["Primary2"]];[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Record2"];#;["Primary2"]];[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]];[["Primary2"];#;["Record2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable` ON (Key) VALUES ('Primary2');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable`;
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "[]");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "[]");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "[]");
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexWithPrimaryKeySameComulns, UseSink) {
        NKikimrConfig::TAppConfig app;
        app.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(app);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("KeyA", EPrimitiveType::Int64)
                .AddNullableColumn("Value1", EPrimitiveType::String)
                .AddNullableColumn("Payload", EPrimitiveType::Utf8);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key", "KeyA"});
            tableBuilder.AddSecondaryIndex("Index1", std::vector<std::string>{"Value1", "KeyA"});
            tableBuilder.AddSecondaryIndex("Index2", std::vector<std::string>{"Key", "Value1"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, KeyA, Value1, Payload) VALUES
                ("Primary1", 42, "Val1", "SomeData");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Primary1"];["Val1"];[42]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1"];["SomeData"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q1_(R"(
                SELECT Key FROM `/Root/TestTable` VIEW Index1 WHERE Value1 = 'Val1';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Primary1\"]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT Key, Value1, Payload FROM `/Root/TestTable` VIEW Index2 WHERE Key = 'Primary1' ORDER BY Key, Value1;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Primary1\"];[\"Val1\"];[\"SomeData\"]]]");
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, KeyA, Value1, Payload) VALUES
                ("Primary1", 42, "Val1_0", "SomeData2");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_0"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Primary1"];["Val1_0"];[42]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_0"];["SomeData2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, KeyA, Value1) VALUES
                ("Primary1", 42, "Val1_1");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];[42]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_1"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, KeyA) VALUES
                ("Primary1", 42);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[#;[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];#;#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` SET Value1 = 'Val1_2';
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_2"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, KeyA, Value1) VALUES ('Primary1', 42, 'Val1_3');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_3"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_3"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, KeyA, Value1) VALUES ('Primary2', 43, 'Val2');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_3"];[42];["Primary1"]];[["Val2"];[43];["Primary2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_3"];#];[["Primary2"];[43];["Val2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable` WHERE Key = 'Primary2';
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_3"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_3"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable` ON (Key, KeyA) VALUES ('Primary1', 42);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "[]");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "[]");
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexUsingInJoin, UseStreamJoin) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableKqpDataQueryStreamIdxLookupJoin(UseStreamJoin);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(appConfig);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::Int64);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value");
            auto result = session.CreateTable("/Root/TestTable1", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::Int64);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value");
            auto result = session.CreateTable("/Root/TestTable2", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable1` (Key, Value) VALUES
                    ("Table1Primary3", 3),
                    ("Table1Primary4", 4),
                    ("Table1Primary55", 55);

                UPSERT INTO `/Root/TestTable2` (Key, Value) VALUES
                    ("Table2Primary1", 1),
                    ("Table2Primary2", 2),
                    ("Table2Primary3", 3),
                    ("Table2Primary4", 4),
                    ("Table2Primary5", 5);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        {
            const TString query(Q1_(R"(
                SELECT `/Root/TestTable1`.Key FROM `/Root/TestTable1`
                    INNER JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t2.Value = `/Root/TestTable1`.Value
                ORDER BY `/Root/TestTable1`.Key;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            // UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary3\"]];[[\"Table1Primary4\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            if (UseStreamJoin) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 2);

                for (const auto& ta : stats.query_phases(0).table_access()) {
                    if (ta.name() == "/Root/TestTable1") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 3);
                    } else {
                        UNIT_ASSERT_VALUES_EQUAL(ta.name(), "/Root/TestTable2/Index1/indexImplTable");
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    }
                }
            } else {
                int indexPhaseId = 1;
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT `/Root/TestTable1`.Key, `/Root/TestTable1`.Value FROM `/Root/TestTable1`
                    INNER JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t2.Value = `/Root/TestTable1`.Value ORDER BY `/Root/TestTable1`.Value DESC;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            // UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary4\"];[4]];[[\"Table1Primary3\"];[3]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            if (UseStreamJoin) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 2);

                for (const auto& ta : stats.query_phases(0).table_access()) {
                    if (ta.name() == "/Root/TestTable1") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 3);
                    } else {
                        UNIT_ASSERT_VALUES_EQUAL(ta.name(), "/Root/TestTable2/Index1/indexImplTable");
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    }
                }
            } else {
                int indexPhaseId = 1;
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT `/Root/TestTable1`.Key FROM `/Root/TestTable1`
                    LEFT JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t2.Value = `/Root/TestTable1`.Value
                ORDER BY `/Root/TestTable1`.Key;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            // UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary3\"]];[[\"Table1Primary4\"]];[[\"Table1Primary55\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            if (UseStreamJoin) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 2);

                for (const auto& ta : stats.query_phases(0).table_access()) {
                    if (ta.name() == "/Root/TestTable1") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 3);
                    } else {
                        UNIT_ASSERT_VALUES_EQUAL(ta.name(), "/Root/TestTable2/Index1/indexImplTable");
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    }
                }
            } else {
                int indexPhaseId = 1;
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT `/Root/TestTable1`.Key, `/Root/TestTable1`.Value FROM `/Root/TestTable1`
                    LEFT JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t2.Value = `/Root/TestTable1`.Value ORDER BY `/Root/TestTable1`.Value DESC;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            // UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary55\"];[55]];[[\"Table1Primary4\"];[4]];[[\"Table1Primary3\"];[3]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            if (UseStreamJoin) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 2);

                for (const auto& ta : stats.query_phases(0).table_access()) {
                    if (ta.name() == "/Root/TestTable1") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 3);
                    } else {
                        UNIT_ASSERT_VALUES_EQUAL(ta.name(), "/Root/TestTable2/Index1/indexImplTable");
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    }
                }
            } else {
                int indexPhaseId = 1;
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
            }
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexUsingInJoin2, UseStreamJoin) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableKqpDataQueryStreamIdxLookupJoin(UseStreamJoin);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(appConfig);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);


        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::Int64);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value");
            auto result = session.CreateTable("/Root/TestTable1", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::Int64)
                .AddNullableColumn("Value2", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value");
            auto result = session.CreateTable("/Root/TestTable2", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable1` (Key, Value) VALUES
                    ("Table1Primary3", 3),
                    ("Table1Primary4", 4),
                    ("Table1Primary55", 55);

                UPSERT INTO `/Root/TestTable2` (Key, Value, Value2) VALUES
                    ("Table2Primary1", 1, "aa"),
                    ("Table2Primary2", 2, "bb"),
                    ("Table2Primary3", 3, "cc"),
                    ("Table2Primary4", 4, "dd"),
                    ("Table2Primary5", 5, "ee");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const TString query(Q1_(R"(
                SELECT t1.Key, t2.Value2 FROM `/Root/TestTable1` as t1
                    INNER JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t1.Value = t2.Value
                ORDER BY t1.Key;
        )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            // UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary3\"];[\"cc\"]];[[\"Table1Primary4\"];[\"dd\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            if (UseStreamJoin) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 3);

                for (const auto& ta : stats.query_phases(0).table_access()) {
                    if (ta.name() == "/Root/TestTable1") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 3);
                    } else if (ta.name() == "/Root/TestTable2") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    } else {
                        UNIT_ASSERT_VALUES_EQUAL(ta.name(), "/Root/TestTable2/Index1/indexImplTable");
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    }
                }
            } else {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 2);
                for (const auto& ta : stats.query_phases(1).table_access()) {
                    if (ta.name() == "/Root/TestTable2") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    } else {
                        UNIT_ASSERT_VALUES_EQUAL(ta.name(), "/Root/TestTable2/Index1/indexImplTable");
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    }
                }
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT t1.Key, t2.Value2 FROM `/Root/TestTable1` as t1
                    LEFT JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t1.Value = t2.Value
                ORDER BY t1.Key;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            // UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary3\"];[\"cc\"]];[[\"Table1Primary4\"];[\"dd\"]];[[\"Table1Primary55\"];#]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            if (UseStreamJoin) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 3);

                for (const auto& ta : stats.query_phases(0).table_access()) {
                    if (ta.name() == "/Root/TestTable1") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 3);
                    } else if (ta.name() == "/Root/TestTable2") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    } else {
                        UNIT_ASSERT_VALUES_EQUAL(ta.name(), "/Root/TestTable2/Index1/indexImplTable");
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    }
                }
            } else {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 2);
                for (const auto& ta : stats.query_phases(1).table_access()) {
                    if (ta.name() == "/Root/TestTable2") {
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    } else {
                        UNIT_ASSERT_VALUES_EQUAL(ta.name(), "/Root/TestTable2/Index1/indexImplTable");
                        UNIT_ASSERT_VALUES_EQUAL(ta.reads().rows(), 2);
                    }
                }
            }
        }
    }

    Y_UNIT_TEST(ForbidViewModification) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1", "Value1");
            )"));

            auto result1 = session.ExecuteDataQuery(
                                     query1,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result1.IsSuccess());
        }

        {
            const TString query(Q1_(R"(
                INSERT INTO `/Root/TestTable` VIEW Index (Index2, Key) VALUES('Secondary2', 'Primary2');
            )"));

            auto result = session.ExecuteDataQuery(
                                 query,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::GENERIC_ERROR);
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(), "mismatched input", result.GetIssues().ToString());
        }

        {
            const TString query(Q1_(R"(
                UPSERT INTO `/Root/TestTable` VIEW Index (Index2, Key) VALUES('Secondary2', 'Primary2');
            )"));

            auto result = session.ExecuteDataQuery(
                                 query,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::GENERIC_ERROR);
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(), "mismatched input", result.GetIssues().ToString());
        }

        {
            const TString query(Q1_(R"(
                UPDATE `/Root/TestTable` VIEW Index ON (Index2, Key) VALUES ('Primary1', 'Secondary1');
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::GENERIC_ERROR);
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(), "mismatched input", result.GetIssues().ToString());
        }

        {
            const TString query(Q1_(R"(
                DELETE FROM `/Root/TestTable` VIEW Index WHERE Index2 = 'Secondary1';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::GENERIC_ERROR);
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(), "mismatched input", result.GetIssues().ToString());
        }

    }

    Y_UNIT_TEST(ForbidDirectIndexTableCreation) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto scheme = kikimr.GetSchemeClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Value", EPrimitiveType::String)
                .AddNullableColumn("Key", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Value"});
            auto result = session.CreateTable("/Root/TestTable/Index", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::GENERIC_ERROR);
        }

        {
            auto result = scheme.MakeDirectory("/Root/TestTable/Index").ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::GENERIC_ERROR);
        }

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Value", EPrimitiveType::String)
                .AddNullableColumn("Key", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Value"});
            auto result = session.CreateTable("/Root/TestTable/Index/indexImplTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::GENERIC_ERROR);
        }
    }

    Y_UNIT_TEST(DuplicateUpsertInterleave) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto tableBuilder = db.GetTableBuilder();
        tableBuilder
            .AddNullableColumn("Key", EPrimitiveType::String)
            .AddNullableColumn("Index2", EPrimitiveType::String)
            .AddNullableColumn("Value", EPrimitiveType::String);
        tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
        tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
        auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

        {
            const TString query(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                    ("Primary1", "Secondary1", "Value1"),
                    ("Primary2", "Secondary2", "Value2"),
                    ("Primary1", "Secondary11", "Value3"),
                    ("Primary2", "Secondary22", "Value3");
            )"));

            auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            CompareYson(R"([[["Secondary11"];["Primary1"]];[["Secondary22"];["Primary2"]]])",
                ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable"));
        }
    }

    Y_UNIT_TEST_TWIN(DuplicateUpsertInterleaveParams, UseSink) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(appConfig);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto tableBuilder = db.GetTableBuilder();
        tableBuilder
            .AddNullableColumn("Key", EPrimitiveType::String)
            .AddNullableColumn("Index2", EPrimitiveType::String)
            .AddNullableColumn("Value", EPrimitiveType::String);
        tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
        tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
        auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

        {
            const TString query(Q1_(R"(
                DECLARE $rows AS
                    List<Struct<
                        Key: String?,
                        Index2: String?,
                        Value: String?>>;

                    UPSERT INTO `/Root/TestTable`
                    SELECT Key, Index2, Value FROM AS_TABLE($rows);
            )"));

            auto explainResult = session.ExplainDataQuery(query).ExtractValueSync();
            UNIT_ASSERT_C(explainResult.IsSuccess(), explainResult.GetIssues().ToString());

            NJson::TJsonValue plan;
            NJson::ReadJsonTree(explainResult.GetPlan(), &plan, true);

            UNIT_ASSERT(plan.GetMapSafe().contains("tables"));
            const auto& tables = plan.GetMapSafe().at("tables").GetArraySafe();
            UNIT_ASSERT(tables.size() == 2);
            UNIT_ASSERT(tables.at(0).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable");
            UNIT_ASSERT(tables.at(1).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable/Index/indexImplTable");

            auto qId = session.PrepareDataQuery(query).ExtractValueSync().GetQuery();

            auto params = qId.GetParamsBuilder()
                .AddParam("$rows")
                .BeginList()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").OptionalString("Primary1")
                        .AddMember("Index2").OptionalString("Secondary1")
                        .AddMember("Value").OptionalString("Value1")
                    .EndStruct()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").OptionalString("Primary2")
                        .AddMember("Index2").OptionalString("Secondary2")
                        .AddMember("Value").OptionalString("Value2")
                    .EndStruct()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").OptionalString("Primary1")
                        .AddMember("Index2").OptionalString("Secondary11")
                        .AddMember("Value").OptionalString("Value1")
                    .EndStruct()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").OptionalString("Primary2")
                        .AddMember("Index2").OptionalString("Secondary22")
                        .AddMember("Value").OptionalString("Value2")
                    .EndStruct()
                .EndList()
                .Build()
            .Build();

            auto result = qId.Execute(
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                    std::move(params)).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            CompareYson(R"([[["Secondary11"];["Primary1"]];[["Secondary22"];["Primary2"]]])",
                ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable"));
        }
    }

    Y_UNIT_TEST(MultipleModifications) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto tableBuilder = db.GetTableBuilder();
        tableBuilder
            .AddNullableColumn("Key", EPrimitiveType::String)
            .AddNullableColumn("Index2", EPrimitiveType::String)
            .AddNullableColumn("Value", EPrimitiveType::String);
        tableBuilder.SetPrimaryKeyColumns(std::vector<std::string>{"Key"});
        tableBuilder.AddSecondaryIndex("Index", std::vector<std::string>{"Index2"});
        auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                    ("Primary1", "Secondary1", "Value1"),
                    ("Primary2", "Secondary2", "Value2");
            )"));

            auto result = session.ExecuteDataQuery(query1, TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            auto tx = result.GetTransaction();


            const TString query2(Q_(R"(
                DELETE FROM `/Root/TestTable` ON (Key) VALUES
                    ("Primary1"),
                    ("Primary2");
            )"));

            result = session.ExecuteDataQuery(query2, TTxControl::Tx(*tx).CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "[]");
        }
    }

    void CreateTableWithIndexSQL(EIndexTypeSql type) {
        auto kqpSetting = NKikimrKqp::TKqpSetting();
        kqpSetting.SetName("_KqpYqlSyntaxVersion");
        kqpSetting.SetValue("1");

        auto settings = TKikimrSettings()
                .SetKqpSettings({kqpSetting});
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const auto typeStr = IndexTypeSqlString(type);
        const TString createTableSql = Sprintf("CREATE TABLE `/Root/TestTable` ("
                "    Key Int32, IndexA Int32, IndexB Int32, IndexC String, Value String,"
                "    PRIMARY KEY (Key),"
                "    INDEX SecondaryIndex1 %s ON (IndexA, IndexB),"
                "    INDEX SecondaryIndex2 %s ON (IndexC)"
                ")", typeStr.data(), typeStr.data());
        {
            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        // Multiple create table requests with same scheme should be OK
        {

            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        // Check we can use index (metadata is correct)
        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, IndexA, IndexB, IndexC, Value) VALUES
            (1, 11, 111, "a", "Value1"),
            (2, 22, 222, "b", "Value2"),
            (3, 33, 333, "c", "Value3");
        )"));

        auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        auto waitForAsyncIndexContent = [&session](const TString& indexImplTable, const TString& expected) {
            while (true) {
                const auto& yson = ReadTablePartToYson(session, indexImplTable);
                if (yson == expected) {
                    break;
                }

                Sleep(TDuration::Seconds(1));
            }
        };

        {
            const TString indexImplTable = "/Root/TestTable/SecondaryIndex1/indexImplTable";
            const TString expected = R"([[[11];[111];[1]];[[22];[222];[2]];[[33];[333];[3]]])";
            waitForAsyncIndexContent(indexImplTable, expected);
        }
        {
            const TString indexImplTable = "/Root/TestTable/SecondaryIndex2/indexImplTable";
            const TString expected = R"([[["a"];[1]];[["b"];[2]];[["c"];[3]]])";
            waitForAsyncIndexContent(indexImplTable, expected);
        }
    }

    Y_UNIT_TEST(CreateTableWithImplicitSyncIndexSQL) {
        CreateTableWithIndexSQL(EIndexTypeSql::Global);
    }

    Y_UNIT_TEST(CreateTableWithExplicitSyncIndexSQL) {
        CreateTableWithIndexSQL(EIndexTypeSql::GlobalSync);
    }

    Y_UNIT_TEST(CreateTableWithExplicitAsyncIndexSQL) {
        CreateTableWithIndexSQL(EIndexTypeSql::GlobalAsync);
    }

    void SelectFromAsyncIndexedTable() {
        auto kqpSetting = NKikimrKqp::TKqpSetting();
        kqpSetting.SetName("_KqpYqlSyntaxVersion");
        kqpSetting.SetValue("1");

        auto settings = TKikimrSettings()
                .SetKqpSettings({kqpSetting});
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto result = session.ExecuteSchemeQuery(R"(
                CREATE TABLE `/Root/TestTable` (
                    Key Int32, Index Int32, Value String,
                    PRIMARY KEY (Key),
                    INDEX SecondaryIndex GLOBAL ASYNC ON (Index)
                )
            )").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        const auto query = Q_("SELECT * FROM `/Root/TestTable` VIEW SecondaryIndex WHERE Index == 1;");
        auto queryId = session.PrepareDataQuery(query).ExtractValueSync().GetQuery();

        const auto variants = TVector<std::pair<TTxSettings, EStatus>>{
            {TTxSettings::SerializableRW(), EStatus::PRECONDITION_FAILED},
            {TTxSettings::OnlineRO(), EStatus::PRECONDITION_FAILED},
            {TTxSettings::StaleRO(), EStatus::SUCCESS},
        };

        for (const auto& [settings, status] : variants) {
            {
                auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx(settings).CommitTx()).ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), status, TStringBuilder() << "Unexpected status #1"
                    << ": expected# " << status
                    << ": got# " << result.GetStatus()
                    << ", settings# " << settings
                    << ", issues# " << result.GetIssues().ToString());
            }

            {
                auto result = queryId.Execute(TTxControl::BeginTx(settings).CommitTx()).ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), status, TStringBuilder() << "Unexpected status #2"
                    << ": expected# " << status
                    << ": got# " << result.GetStatus()
                    << ", settings# " << settings
                    << ", issues# " << result.GetIssues().ToString());
            }
        }
    }

    Y_UNIT_TEST(SelectFromAsyncIndexedTable) {
        SelectFromAsyncIndexedTable();
    }

    Y_UNIT_TEST(SelectFromIndexesAndFreeSpaceLogicDoesntTimeout) {
        auto setting = NKikimrKqp::TKqpSetting();
        setting.SetName("_KqpYqlSyntaxVersion");
        setting.SetValue("1");
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});

        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableKqpDataQueryStreamIdxLookupJoin(true);
        // setting channel buffer size so small to make sure that we will be able to transfer at least
        // one row in stream lookup.
        appConfig.MutableTableServiceConfig()->MutableResourceManager()->SetChannelBufferSize(1_KB);
        // setting string a bit larger than size of the channel buffer.
        const int payloadSize = 5000;

        serverSettings.SetAppConfig(appConfig);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        {
            const TString query(Q_(R"(
                DECLARE $Payload AS String;
                REPLACE INTO `/Root/SecondaryComplexKeys` (Key, Fk1, Fk2, Value) VALUES
                    (1,    1,    "Fk1", $Payload);
            )"));

            TString largeString(payloadSize, 'a');

            auto params = TParamsBuilder()
                .AddParam("$Payload")
                    .String(largeString)
                    .Build()
                .Build();

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx().CommitTx(),
                params,
                execSettings).ExtractValueSync();

            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const TString query1(Q_(R"(
                SELECT *
                FROM `/Root/SecondaryComplexKeys` VIEW Index
                WHERE Fk1 = 1
                LIMIT 10;
            )"));

            auto result2 = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx().CommitTx(),
                execSettings).ExtractValueSync();

            UNIT_ASSERT_C(result2.IsSuccess(), result2.GetIssues().ToString());
            // UNIT_ASSERT(result2.GetIssues().Empty());
        }

        {
            const TString query1(Q_(R"(
                SELECT q.Value as V1, t.Value as V2
                FROM `/Root/SecondaryComplexKeys` VIEW Index as t
                LEFT JOIN `/Root/SecondaryComplexKeys` as q
                ON q.Key = t.Key
                WHERE t.Key = 1
                LIMIT 10;
            )"));

            auto result2 = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx().CommitTx(),
                execSettings).ExtractValueSync();

            UNIT_ASSERT_C(result2.IsSuccess(), result2.GetIssues().ToString());
            // UNIT_ASSERT(result2.GetIssues().Empty());
        }
    }

    Y_UNIT_TEST(InnerJoinWithNonIndexWherePredicate) {
        auto setting = NKikimrKqp::TKqpSetting();
        setting.SetName("_KqpYqlSyntaxVersion");
        setting.SetValue("1");
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        {
            const TString query1(Q_(R"(
                DECLARE $mongo_key AS Utf8?;
                DECLARE $targets AS List<Struct<fk1:Int32>>;
                DECLARE $limit AS Uint64;

                SELECT t.Value AS value
                FROM AS_TABLE($targets) AS k
                INNER JOIN `/Root/SecondaryComplexKeys` VIEW Index AS t
                ON t.Fk1 = k.fk1
                WHERE Fk2 > $mongo_key
                LIMIT $limit;
            )"));

            auto result = session.ExplainDataQuery(
                query1)
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            UNIT_ASSERT_C(!result.GetAst().contains("EquiJoin"), result.GetAst());

            auto params = TParamsBuilder()
                .AddParam("$targets")
                    .BeginList()
                        .AddListItem()
                            .BeginStruct()
                                .AddMember("fk1").Int32(1)
                            .EndStruct()
                    .EndList().Build()
                .AddParam("$mongo_key")
                    .OptionalUtf8("")
                    .Build()
                .AddParam("$limit")
                    .Uint64(2)
                    .Build()
                .Build();


            auto result2 = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx().CommitTx(),
                params,
                execSettings).ExtractValueSync();

            UNIT_ASSERT_C(result2.IsSuccess(), result2.GetIssues().ToString());
            // UNIT_ASSERT(result2.GetIssues().Empty());

            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result2.GetResultSet(0)), "[[[\"Payload1\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result2.GetStats());

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/SecondaryComplexKeys");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(1).name(), "/Root/SecondaryComplexKeys/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(1).reads().rows(), 1);
        }
    }

    //KIKIMR-8144
    Y_UNIT_TEST(InnerJoinSecondaryIndexLookupAndRightTablePredicateNonIndexColumn) {
        auto setting = NKikimrKqp::TKqpSetting();
        setting.SetName("_KqpYqlSyntaxVersion");
        setting.SetValue("1");
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const TString createTableSql = R"(CREATE TABLE `/Root/user` (
            id Uint64,
            uid Uint64,
            yandexuid Utf8,
            PRIMARY KEY (id),
            INDEX uid_index GLOBAL ON (uid),
            INDEX yandexuid_index GLOBAL ON (yandexuid)
        );)";
        {
            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/user` (id, yandexuid, uid) VALUES
                (2, "def", 222),
                (1, "abc", NULL),
                (NULL, "ghi", 333);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();

            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/user");
            const TString expected = R"([[#;[333u];["ghi"]];[[1u];#;["abc"]];[[2u];[222u];["def"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        auto execQuery = [&session](const TString& query) {
            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            return NYdb::FormatResultSetYson(result.GetResultSet(0));
        };

        const TString query_template(Q_(R"(
            $yandexuids = (AsList(AsStruct(CAST("abc" as Utf8) as yandexuid), AsStruct(CAST("def" as Utf8) as yandexuid)));
            SELECT t.id as id, t.yandexuid as yandexuid, t.uid as uid
                FROM AS_TABLE($yandexuids) AS k
                %s JOIN `/Root/user` VIEW yandexuid_index AS t
                ON t.yandexuid = k.yandexuid
                %s
            ;)"));

        {
            const TString query = Sprintf(query_template.data(), "INNER", "WHERE uid IS NULL");
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "INNER", "WHERE uid IS NOT NULL");
            const TString expected = R"([[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "LEFT", "WHERE uid IS NULL");
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "LEFT", "WHERE uid IS NOT NULL");
            const TString expected = R"([[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NULL");
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY id DESC");
            const TString expected = R"([[[2u];["def"];[222u]];[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY id");
            const TString expected = R"([[#;["ghi"];[333u]];[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY yandexuid");
            const TString expected = R"([[[2u];["def"];[222u]];[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY yandexuid DESC");
            const TString expected = R"([[#;["ghi"];[333u]];[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY uid");
            const TString expected = R"([[[2u];["def"];[222u]];[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY uid DESC");
            const TString expected = R"([[#;["ghi"];[333u]];[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL");
            const TString expected = R"([[[2u];["def"];[222u]];[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT ONLY", "WHERE uid IS NULL");
            const TString expected = R"([])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT ONLY", "WHERE uid IS NOT NULL");
            const TString expected = R"([[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT SEMI", "WHERE uid IS NULL");
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT SEMI", "WHERE uid IS NOT NULL");
            const TString expected = R"([[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        // Without using index by pk directly
        {
            const TString query1(Q_(R"(
                $ids = (AsList(AsStruct(CAST(1 as Uint64) as id), AsStruct(CAST(2 as Uint64) as id)));

                SELECT t.id as id, t.yandexuid as yandexuid, t.uid as uid
                    FROM AS_TABLE($ids) AS k
                    INNER JOIN `/Root/user` VIEW PRIMARY KEY AS t
                    ON t.id = k.id
                    WHERE uid IS NULL
                ;)"));
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query1), expected);
        }

        {
            const TString query1(Q_(R"(
                $ids = (AsList(AsStruct(CAST(1 as Uint64) as id), AsStruct(CAST(2 as Uint64) as id)));

                SELECT t.id as id, t.yandexuid as yandexuid, t.uid as uid
                    FROM AS_TABLE($ids) AS k
                    INNER JOIN `/Root/user` AS t
                    ON t.id = k.id
                    WHERE uid IS NULL
                ;)"));
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query1), expected);
        }
    }

    Y_UNIT_TEST(DeleteByIndex) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        

        TScriptingClient client(kikimr.GetDriver());
        auto scriptResult = client.ExecuteYqlScript(R"(
            --!syntax_v1
            CREATE TABLE TestTable (
                Key Int32,
                Subkey Utf8,
                Value Utf8,
                PRIMARY KEY (Key, Subkey),
                INDEX SecondaryIndex GLOBAL ON (Subkey)
            );

            COMMIT;

            UPSERT INTO TestTable (Key, Subkey, Value) VALUES
                (1, "One", "Value1"),
                (1, "Two", "Value2"),
                (2, "One", "Value3"),
                (3, "One", "Value4");
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(scriptResult.GetStatus(), EStatus::SUCCESS, scriptResult.GetIssues().ToString());

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto query = Q1_(R"(
            DECLARE $subkey AS Utf8;
            DECLARE $keys AS List<Int32>;

            $to_delete =
                SELECT Key FROM TestTable VIEW SecondaryIndex
                WHERE Subkey = $subkey AND Key NOT IN $keys;

            DELETE FROM TestTable
            WHERE Key IN $to_delete;
        )");

        auto explainResult = session.ExplainDataQuery(query).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(explainResult.GetStatus(), EStatus::SUCCESS, explainResult.GetIssues().ToString());

        NJson::TJsonValue plan;
        NJson::ReadJsonTree(explainResult.GetPlan(), &plan, true);
        // NJson::WriteJson(&Cerr, &plan["tables"], true);

        auto tablePlan = FindPlanNodeByKv(plan["tables"], "name", "/Root/TestTable");
        UNIT_ASSERT(tablePlan.IsDefined());
        // TODO: KIKIMR-14074 (Unnecessary left semi join with own index table)
        UNIT_ASSERT_VALUES_EQUAL(tablePlan["reads"].GetArraySafe().size(), 1);
        for (const auto& read : tablePlan["reads"].GetArraySafe()) {
            UNIT_ASSERT_VALUES_UNEQUAL(read["type"].GetString(), "");
            UNIT_ASSERT_VALUES_UNEQUAL(read["type"].GetString(), "FullScan");
        }

        auto indexPlan = FindPlanNodeByKv(plan["tables"], "name", "/Root/TestTable/SecondaryIndex/indexImplTable");
        UNIT_ASSERT(indexPlan.IsDefined());
        for (const auto& read : indexPlan["reads"].GetArraySafe()) {
            UNIT_ASSERT_VALUES_UNEQUAL(read["type"].GetString(), "");
            UNIT_ASSERT_VALUES_UNEQUAL(read["type"].GetString(), "FullScan");
        }

        auto params = db.GetParamsBuilder()
            .AddParam("$subkey")
                .Utf8("One")
                .Build()
            .AddParam("$keys")
                .BeginList()
                    .AddListItem().Int32(1)
                .EndList()
                .Build()
            .Build();

        auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx(), params).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT * FROM TestTable ORDER BY Key, Subkey;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

        CompareYson(R"([
            [[1];["One"];["Value1"]];
            [[1];["Two"];["Value2"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST_TWIN(UpdateDeletePlan, UseSink) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableOltpSink(UseSink);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(appConfig);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto schemeResult = session.ExecuteSchemeQuery(R"(
            --!syntax_v1
            CREATE TABLE TestTable (
                Key Int32,
                Subkey Utf8,
                Value Utf8,
                PRIMARY KEY (Key),
                INDEX SecondaryIndex GLOBAL ON (Subkey)
            );
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(schemeResult.GetStatus(), EStatus::SUCCESS, schemeResult.GetIssues().ToString());

        auto checkPlan = [](const std::string& planJson, ui32 tableReads, ui32 tableWrites, TMaybe<ui32> indexWrites) {
            NJson::TJsonValue plan;
            NJson::ReadJsonTree(planJson, &plan, true);
            const auto& tables = plan["tables"];
            // NJson::WriteJson(&Cerr, &tables, true);

            auto tablePlan = FindPlanNodeByKv(tables, "name", "/Root/TestTable");
            auto indexPlan = FindPlanNodeByKv(tables, "name", "/Root/TestTable/SecondaryIndex/indexImplTable");

            UNIT_ASSERT_VALUES_EQUAL(tablePlan["reads"].GetArraySafe().size(), tableReads);
            UNIT_ASSERT_VALUES_EQUAL(tablePlan["writes"].GetArraySafe().size(), tableWrites);
            if (indexWrites) {
                UNIT_ASSERT_VALUES_EQUAL(indexPlan["writes"].GetArraySafe().size(), *indexWrites);
            }
        };

        auto result = session.ExplainDataQuery(Q1_(R"(
            UPDATE TestTable SET Subkey = "Updated" WHERE Value = "Value2";
        )")).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        checkPlan(result.GetPlan(), 1, 1, 2);

        result = session.ExplainDataQuery(Q1_(R"(
            UPDATE TestTable SET Value = "Updated" WHERE Value = "Value2";
        )")).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        checkPlan(result.GetPlan(), 1, 1, {});

        result = session.ExplainDataQuery(Q1_(R"(
            DELETE FROM TestTable WHERE Value = "Value2";
        )")).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        checkPlan(result.GetPlan(), 1, 1, 1);
    }

    Y_UNIT_TEST(UpsertNoIndexColumns) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        auto params = db.GetParamsBuilder()
            .AddParam("$rows")
                .BeginList()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").Int32(2)
                        .AddMember("Value").String("Upsert2")
                    .EndStruct()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").Int32(3)
                        .AddMember("Value").String("Upsert3")
                    .EndStruct()
                .EndList()
                .Build()
            .Build();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $rows AS List<Struct<'Key': Int32, 'Value': String>>;

            UPSERT INTO SecondaryKeys
            SELECT * FROM AS_TABLE($rows);
        )"), TTxControl::BeginTx().CommitTx(), params).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT Key, Fk, Value FROM SecondaryKeys WHERE Key IN [2, 3] ORDER BY Key;
            SELECT Key FROM SecondaryKeys VIEW Index WHERE Fk IS NULL ORDER BY Key;
            SELECT Key FROM SecondaryKeys VIEW Index WHERE Fk = 2 ORDER BY Key;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

        CompareYson(R"([
            [[2];[2];["Upsert2"]];
            [[3];#;["Upsert3"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));

        CompareYson(R"([[#];[[3]];[[7]]])", FormatResultSetYson(result.GetResultSet(1)));
        CompareYson(R"([[[2]]])", FormatResultSetYson(result.GetResultSet(2)));
    }

    Y_UNIT_TEST(UpdateIndexSubsetPk) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        TScriptingClient client(kikimr.GetDriver());
        auto scriptResult = client.ExecuteYqlScript(R"(
            --!syntax_v1
            CREATE TABLE TestTable (
                Key1 Int32,
                Key2 Uint64,
                Key3 String,
                Value String,
                PRIMARY KEY (Key1, Key2, Key3),
                INDEX SecondaryIndex GLOBAL ON (Key1, Key3)
            );

            COMMIT;

            UPSERT INTO TestTable (Key1, Key2, Key3, Value) VALUES
                (1, 10, "One", "Value1"),
                (1, 20, "Two", "Value2"),
                (2, 30, "One", "Value3"),
                (2, 40, "Two", "Value4");
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(scriptResult.GetStatus(), EStatus::SUCCESS, scriptResult.GetIssues().ToString());

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            UPDATE TestTable ON
            SELECT Key1, Key2, Key3, "Updated" AS Value
            FROM TestTable VIEW SecondaryIndex
            WHERE Key1 = 1 AND Key3 = "Two";
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/TestTable", {
            .ExpectedReads = 0,
            .ExpectedUpdates = 1
        });

        AssertTableStats(result, "/Root/TestTable/SecondaryIndex/indexImplTable", {
            .ExpectedReads = 1,
            .ExpectedUpdates = 0
        });

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT * FROM TestTable VIEW SecondaryIndex WHERE Key1 = 1 ORDER BY Key1, Key2, Key3;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

        CompareYson(R"([
            [[1];[10u];["One"];["Value1"]];
            [[1];[20u];["Two"];["Updated"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));

        result = session.ExecuteDataQuery(Q1_(R"(
            UPDATE TestTable SET Value = "Updated2"
            WHERE Key1 = 2 AND Key2 = 30;
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/TestTable", {
            .ExpectedReads = 1,
            .ExpectedUpdates = 1
        });

        AssertTableStats(result, "/Root/TestTable/SecondaryIndex/indexImplTable", {
            .ExpectedReads = 0,
            .ExpectedUpdates = 0
        });

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT * FROM TestTable VIEW SecondaryIndex WHERE Key1 = 2 ORDER BY Key1, Key2, Key3;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

        CompareYson(R"([
            [[2];[30u];["One"];["Updated2"]];
            [[2];[40u];["Two"];["Value4"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    /* Doesn't work with readranges because of limits/column sets
    Y_UNIT_TEST(IndexMultipleRead) {
        TKikimrRunner kikimr;

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = db.GetParamsBuilder()
            .AddParam("$fks")
                .BeginList()
                .AddListItem().Int32(5)
                .AddListItem().Int32(10)
                .EndList()
                .Build()
            .Build();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $fks AS List<Int32>;

            SELECT * FROM SecondaryKeys VIEW Index WHERE Fk IN $fks;
            SELECT COUNT(*) FROM SecondaryKeys VIEW Index WHERE Fk IN $fks;
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/SecondaryKeys", {
            .ExpectedReads = 1
        });

        AssertTableStats(result, "/Root/SecondaryKeys/Index/indexImplTable", {
            .ExpectedReads = 1,
        });

        CompareYson(R"([[[5];[5];["Payload5"]]])", FormatResultSetYson(result.GetResultSet(0)));
        CompareYson(R"([[1u]])", FormatResultSetYson(result.GetResultSet(1)));
    }
    */

    Y_UNIT_TEST(IndexOr) {
        TKikimrRunner kikimr;

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = db.GetParamsBuilder()
            .AddParam("$A")
                .Int32(1)
                .Build()
            .AddParam("$B")
                .Int32(5)
                .Build()
            .Build();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $A AS Int32;
            DECLARE $B AS Int32;

            SELECT * FROM SecondaryKeys VIEW Index WHERE Fk = $A OR Fk = $B;
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/SecondaryKeys", {
            .ExpectedReads = 2
        });

        AssertTableStats(result, "/Root/SecondaryKeys/Index/indexImplTable", {
            .ExpectedReads = 2,
        });

        CompareYson(R"([[[1];[1];["Payload1"]];[[5];[5];["Payload5"]]])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(IndexFilterPushDown) {
        TKikimrRunner kikimr;

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session, false /*populateTables*/);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            REPLACE INTO `/Root/SecondaryKeys` (Key, Fk, Value) VALUES
                (0,    0,    "Value0"),
                (1,    0,    "Value1"),
                (2,    1,    "Value2");
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT Fk, Value FROM `/Root/SecondaryKeys` VIEW Index WHERE Fk = CAST(0 AS UInt32) LIMIT 1;
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/SecondaryKeys", {
            .ExpectedReads = 1
        });

        AssertTableStats(result, "/Root/SecondaryKeys/Index/indexImplTable", {
            .ExpectedReads = 1
        });

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT Fk, Value FROM `/Root/SecondaryKeys` VIEW Index WHERE Fk = CAST(0 AS UInt32) AND Fk + Fk >= 0 LIMIT 1;
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/SecondaryKeys", {
            .ExpectedReads = 1
        });

        AssertTableStats(result, "/Root/SecondaryKeys/Index/indexImplTable", {
            .ExpectedReads = 1
        });

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT Fk, Value FROM `/Root/SecondaryKeys` VIEW Index WHERE Fk = CAST(0 AS UInt32) ORDER BY Fk LIMIT 1;
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/SecondaryKeys", {
            .ExpectedReads = 1
        });

        AssertTableStats(result, "/Root/SecondaryKeys/Index/indexImplTable", {
            .ExpectedReads = 1
        });

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT Fk, Value FROM `/Root/SecondaryKeys` VIEW Index WHERE Fk = CAST(0 AS UInt32) AND Fk + Fk >= 0 ORDER BY Fk LIMIT 1;
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/SecondaryKeys", {
            .ExpectedReads = 1
        });

        AssertTableStats(result, "/Root/SecondaryKeys/Index/indexImplTable", {
            .ExpectedReads = 1
        });
    }
    Y_UNIT_TEST(IndexTopSortPushDown) {
        TKikimrRunner kikimr;

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session, false /*populateTables*/);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            REPLACE INTO `/Root/SecondaryWithDataColumns` (Key, Index2, Value) VALUES
                ("0",    "0",    "Value2"),
                ("1",    "0",    "Value1"),
                ("2",    "1",    "Value0"),
                ("3",    "1",    "Value0"),
                ("4",    "1",    "Value0");
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        {
            // Use top without sort if ORDER BY is index key columns
            result = session.ExecuteDataQuery(Q1_(R"(
                SELECT Index2, Key, Value, ExtPayload FROM `/Root/SecondaryWithDataColumns` VIEW Index
                ORDER BY Index2, Key
                LIMIT 2;
            )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            AssertTableStats(result, "/Root/SecondaryWithDataColumns", {
                .ExpectedReads = 2
            });
            AssertTableStats(result, "/Root/SecondaryWithDataColumns/Index/indexImplTable", {
                .ExpectedReads = 2
            });
            CompareYson(R"([
                [["0"];["0"];["Value2"];#];
                [["0"];["1"];["Value1"];#]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
        {
            // Use top without sort if ORDER BY is prefix of index key columns
            result = session.ExecuteDataQuery(Q1_(R"(
                SELECT Index2, Key, Value, ExtPayload FROM `/Root/SecondaryWithDataColumns` VIEW Index
                ORDER BY Index2
                LIMIT 2;
            )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            AssertTableStats(result, "/Root/SecondaryWithDataColumns", {
                .ExpectedReads = 2
            });
            AssertTableStats(result, "/Root/SecondaryWithDataColumns/Index/indexImplTable", {
                .ExpectedReads = 2
            });
        }
        {
            // Use push down for top sort if ORDER BY exist column from idex data columns
            result = session.ExecuteDataQuery(Q1_(R"(
                SELECT Index2, Key, Value, ExtPayload FROM `/Root/SecondaryWithDataColumns` VIEW Index
                ORDER BY Index2, Key, Value
                LIMIT 2;
            )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            AssertTableStats(result, "/Root/SecondaryWithDataColumns", {
                .ExpectedReads = 2
            });
            AssertTableStats(result, "/Root/SecondaryWithDataColumns/Index/indexImplTable", {
                .ExpectedReads = 5
            });
        }
        {
            // Use push down for top sort if ORDER BY exist column from idex data columns
            result = session.ExecuteDataQuery(Q1_(R"(
                SELECT * FROM `/Root/SecondaryWithDataColumns` VIEW Index
                ORDER BY Index2, Value
                LIMIT 2;
            )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            AssertTableStats(result, "/Root/SecondaryWithDataColumns", {
                .ExpectedReads = 2
            });
            AssertTableStats(result, "/Root/SecondaryWithDataColumns/Index/indexImplTable", {
                .ExpectedReads = 5
            });
        }
        {
            // Use push down for top sort if all columns in ORDER BY are in data columns
            result = session.ExecuteDataQuery(Q1_(R"(
                SELECT * FROM `/Root/SecondaryWithDataColumns` VIEW Index
                ORDER BY Value
                LIMIT 2;
            )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            AssertTableStats(result, "/Root/SecondaryWithDataColumns", {
                .ExpectedReads = 2
            });
            AssertTableStats(result, "/Root/SecondaryWithDataColumns/Index/indexImplTable", {
                .ExpectedReads = 5
            });
        }
        {
            // Use push down for top sort if columns in ORDER BY in wrong order
            result = session.ExecuteDataQuery(Q1_(R"(
                SELECT * FROM `/Root/SecondaryWithDataColumns` VIEW Index
                ORDER BY Key, Index2, Value
                LIMIT 2;
            )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            AssertTableStats(result, "/Root/SecondaryWithDataColumns", {
                .ExpectedReads = 2
            });
            AssertTableStats(result, "/Root/SecondaryWithDataColumns/Index/indexImplTable", {
                .ExpectedReads = 5
            });
        }
        {
            // Don't use push down for top sort if columns in ORDER BY in wrong order
            result = session.ExecuteDataQuery(Q1_(R"(
                SELECT * FROM `/Root/SecondaryWithDataColumns` VIEW Index
                ORDER BY Index2, Value, Key
                LIMIT 2;
            )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            AssertTableStats(result, "/Root/SecondaryWithDataColumns", {
                .ExpectedReads = 2
            });
            AssertTableStats(result, "/Root/SecondaryWithDataColumns/Index/indexImplTable", {
                .ExpectedReads = 5
            });
        }
        {
            // Don't use push down if ORDER BY exists column not from index
            result = session.ExecuteDataQuery(Q1_(R"(
                SELECT * FROM `/Root/SecondaryWithDataColumns` VIEW Index
                ORDER BY Index2, Key, Value, ExtPayload
                LIMIT 2;
            )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            AssertTableStats(result, "/Root/SecondaryWithDataColumns", {
                .ExpectedReads = 5
            });
            AssertTableStats(result, "/Root/SecondaryWithDataColumns/Index/indexImplTable", {
                .ExpectedReads = 5
            });
        }
    }
    Y_UNIT_TEST(UpdateOnReadColumns) {
        {
            // Check that keys from non involved index are not in read columns
            TKikimrRunner kikimr;
            auto db = kikimr.GetTableClient();
            auto session = db.CreateSession().GetValueSync().GetSession();
            CreateSampleTablesWithIndex(session);

            auto result = session.ExplainDataQuery(R"(
                UPDATE `/Root/SecondaryKeys` ON (Key, Value) VALUES (1, "New");
            )").ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            NJson::TJsonValue plan;
            NJson::ReadJsonTree(result.GetPlan(), &plan, true);
            auto table = plan["tables"][0];
            UNIT_ASSERT_VALUES_EQUAL(table["name"], "/Root/SecondaryKeys");
            UNIT_ASSERT(!table.Has("reads"));
        }
        {
            // Check that keys from involved index are in read columns
            TKikimrRunner kikimr;
            auto db = kikimr.GetTableClient();
            auto session = db.CreateSession().GetValueSync().GetSession();
            CreateSampleTablesWithIndex(session);

            auto result = session.ExplainDataQuery(R"(
                UPDATE `/Root/SecondaryComplexKeys` ON (Key, Fk1, Value) VALUES (1, 1, "New");
            )").ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            NJson::TJsonValue plan;
            NJson::ReadJsonTree(result.GetPlan(), &plan, true);
            auto table = plan["tables"][0];
            UNIT_ASSERT_VALUES_EQUAL(table["name"], "/Root/SecondaryComplexKeys");
            auto reads = table["reads"].GetArraySafe();
            UNIT_ASSERT_VALUES_EQUAL(reads.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(reads[0]["type"], "Lookup");
            UNIT_ASSERT_VALUES_EQUAL(reads[0]["columns"].GetArraySafe().size(), 3);
        }
        {
            // Check that all keys from involved index are in read columns
            TKikimrRunner kikimr;
            auto db = kikimr.GetTableClient();
            auto session = db.CreateSession().GetValueSync().GetSession();
            CreateSampleTablesWithIndex(session);

            auto result = session.ExplainDataQuery(R"(
                UPDATE `/Root/SecondaryComplexKeys` ON (Key, Fk1, Value) VALUES (1, 1, "New");
            )").ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            NJson::TJsonValue plan;
            NJson::ReadJsonTree(result.GetPlan(), &plan, true);
            auto table = plan["tables"][0];
            UNIT_ASSERT_VALUES_EQUAL(table["name"], "/Root/SecondaryComplexKeys");
            auto reads = table["reads"].GetArraySafe();
            UNIT_ASSERT_VALUES_EQUAL(reads.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(reads[0]["type"], "Lookup");
            UNIT_ASSERT_VALUES_EQUAL(reads[0]["columns"].GetArraySafe().size(), 3);
        }
        {
            // Check that data colomns from involved index are in read columns
            TKikimrRunner kikimr;
            auto db = kikimr.GetTableClient();
            auto session = db.CreateSession().GetValueSync().GetSession();
            CreateSampleTablesWithIndex(session);

            auto result = session.ExplainDataQuery(R"(
                UPDATE `/Root/SecondaryWithDataColumns` ON (Key, Value) VALUES ("1", "New");
            )").ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            NJson::TJsonValue plan;
            NJson::ReadJsonTree(result.GetPlan(), &plan, true);
            auto table = plan["tables"][0];
            UNIT_ASSERT_VALUES_EQUAL(table["name"], "/Root/SecondaryWithDataColumns");
            auto reads = table["reads"].GetArraySafe();
            UNIT_ASSERT_VALUES_EQUAL(reads.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(reads[0]["type"], "Lookup");
            UNIT_ASSERT_VALUES_EQUAL(reads[0]["columns"].GetArraySafe().size(), 3);
        }
        {
            // Check that data colomns not from involved index aren't in read columns
            TKikimrRunner kikimr;
            auto db = kikimr.GetTableClient();
            auto session = db.CreateSession().GetValueSync().GetSession();
            CreateSampleTablesWithIndex(session);

            auto result = session.ExplainDataQuery(R"(
                UPDATE `/Root/SecondaryWithDataColumns` ON (Key, ExtPayload) VALUES ("1", "New");
            )").ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            NJson::TJsonValue plan;
            NJson::ReadJsonTree(result.GetPlan(), &plan, true);
            auto table = plan["tables"][0];
            UNIT_ASSERT_VALUES_EQUAL(table["name"], "/Root/SecondaryWithDataColumns");
            UNIT_ASSERT(!table.Has("reads"));
        }
    }

    Y_UNIT_TEST(Uint8Index) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            const TString createTableSql = R"(CREATE TABLE `/Root/table` (
                key Uint8,
                value Uint8,
                PRIMARY KEY (key)
            );)";

            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString upsertSql(Q_(R"(
                UPSERT INTO `/Root/table` (key, value) VALUES
                (0, 1),
                (10, 11),
                (100, 101),
                (200, 201);
            )"));

            auto result = session.ExecuteDataQuery(upsertSql, TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const TString createTableSql = R"(ALTER TABLE `/Root/table`
                ADD INDEX value_index GLOBAL ON (value)
            )";

            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/table");
            const TString expected = R"([[[0u];[1u]];[[10u];[11u]];[[100u];[101u]];[[200u];[201u]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString selectSql(Q1_(R"(
                SELECT * FROM `/Root/table` VIEW value_index WHERE value > 100;
            )"));

            auto result = session.ExecuteDataQuery(selectSql, TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), R"([[[100u];[101u]];[[200u];[201u]]])");
        }
    }

    Y_UNIT_TEST_TWIN(JoinWithNonPKColumnsInPredicate, UseStreamJoin) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetEnableKqpDataQueryStreamIdxLookupJoin(UseStreamJoin);
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting})
            .SetAppConfig(appConfig);
        TKikimrRunner kikimr(serverSettings);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::BUILD_INDEX, NActors::NLog::PRI_TRACE);
        kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {  // create tables
            const TString createTableSql = R"(
                CREATE TABLE `Root/tab1` (
                    id Text NOT NULL,
                    dst_ident Text,
                    exec_dt Timestamp,
                    PRIMARY KEY(id),
                    INDEX ix_exec_dt GLOBAL ON (exec_dt)
                );
                CREATE TABLE `Root/tab2` (
                    id Text NOT NULL,
                    int_ref Text,
                    ext_ref Text,
                    send_dttm Timestamp,
                    PRIMARY KEY(id),
                    INDEX ix_int_ref GLOBAL ON (int_ref),
                    INDEX ix_ext_ref GLOBAL ON (ext_ref),
                    INDEX ix_send_dttm GLOBAL ON (send_dttm)
                );

                CREATE TABLE `Root/tab3` (
                    id Text NOT NULL,
                    dst_ident Text,
                    exec_dt Timestamp,
                    PRIMARY KEY(id),
                    INDEX ix_exec_dt GLOBAL ON (exec_dt)
                );

                CREATE TABLE `Root/tab4` (
                    id Text NOT NULL,
                    int_ref Text,
                    ext_ref Text,
                    good_sign Text,
                    send_dttm Timestamp,
                    PRIMARY KEY(id),
                    INDEX ix_int_ref GLOBAL ON (int_ref),
                    INDEX ix_ext_ref GLOBAL ON (ext_ref),
                    INDEX ix_send_dttm GLOBAL ON (send_dttm)
                );

                CREATE TABLE `Root/tab5` (
                    t5_id Text NOT NULL,
                    t5_coll Text,
                    t5_exec_dt Timestamp,
                    PRIMARY KEY(t5_id),
                    INDEX ix_exec_dt GLOBAL ON (t5_exec_dt),
                    INDEX ix_ref_coll GLOBAL ON (t5_coll)
                );

                CREATE TABLE `Root/tab6` (
                    t6_id Text NOT NULL,
                    t6_coll Text NOT NULL,
                    t6_link_type Text,
                    PRIMARY KEY(t6_id, t6_coll),
                    INDEX ix_magic GLOBAL ON (t6_id, t6_link_type)
                );
            )";

            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {  // fill tables
            const TString upsertSql(Q_(R"(
                UPSERT INTO `Root/tab1` (id, dst_ident, exec_dt) VALUES
                    ('t1-0'u, 'id-1'u, Timestamp('2024-12-03T01:00:00.000000Z')),
                    ('t1-1'u, 'id-2'u, Timestamp('2024-12-03T02:00:00.000000Z')),
                    ('t1-2'u, 'id-3'u, Timestamp('2024-12-03T03:00:00.000000Z'));

                UPSERT INTO `Root/tab2` (id, int_ref, ext_ref, send_dttm) VALUES
                    ('t2-0'u, 't1-0'u, 'id-1'u, Timestamp('2024-12-03T01:01:00.000000Z')),
                    ('t2-1'u, 't1-0'u, null, Timestamp('2024-12-03T02:01:00.000000Z')),
                    ('t2-2'u, 't1-0'u, null, Timestamp('2024-12-03T02:01:00.000000Z')),
                    ('t2-3'u, 't1-1'u, 'id-2'u, Timestamp('2024-12-03T03:01:00.000000Z')),
                    ('t2-4'u, 't1-1'u, null, Timestamp('2024-12-03T04:01:00.000000Z')),
                    ('t2-5'u, 't1-1'u, null, Timestamp('2024-12-03T05:01:00.000000Z')),
                    ('t2-6'u, 't1-2'u, 'id-3'u, Timestamp('2024-12-03T06:01:00.000000Z')),
                    ('t2-7'u, 't1-2'u, null, Timestamp('2024-12-03T07:01:00.000000Z')),
                    ('t2-8'u, 't1-2'u, null, Timestamp('2024-12-03T08:01:00.000000Z')),
                    ('t2-9'u, 't1-2'u, null, Timestamp('2024-12-03T09:01:00.000000Z'));

                UPSERT INTO `Root/tab3` (id, dst_ident, exec_dt) VALUES
                    ('t1-0'u, 'id-1'u, Timestamp('2024-12-03T01:00:00.000000Z')),
                    ('t1-1'u, 'id-2'u, Timestamp('2024-12-03T02:00:00.000000Z')),
                    ('t1-2'u, 'id-3'u, Timestamp('2024-12-03T03:00:00.000000Z'));

                UPSERT INTO `Root/tab4` (id, int_ref, ext_ref, good_sign, send_dttm) VALUES
                    ('t2-0'u, 't1-0'u, 'id-1'u, 'GOOD'u, Timestamp('2024-12-03T01:01:00.000000Z')),
                    ('t2-1'u, 't1-0'u, null, 'BAD'u, Timestamp('2024-12-03T02:01:00.000000Z')),
                    ('t2-2'u, 't1-0'u, null, 'BAD'u, Timestamp('2024-12-03T02:01:00.000000Z')),
                    ('t2-3'u, 't1-1'u, 'id-2'u, 'GOOD'u, Timestamp('2024-12-03T03:01:00.000000Z')),
                    ('t2-4'u, 't1-1'u, null, 'BAD'u, Timestamp('2024-12-03T04:01:00.000000Z')),
                    ('t2-5'u, 't1-1'u, null, 'BAD'u, Timestamp('2024-12-03T05:01:00.000000Z')),
                    ('t2-6'u, 't1-2'u, 'id-3'u, 'GOOD'u, Timestamp('2024-12-03T06:01:00.000000Z')),
                    ('t2-7'u, 't1-2'u, null, 'BAD'u, Timestamp('2024-12-03T07:01:00.000000Z')),
                    ('t2-8'u, 't1-2'u, null, 'BAD'u, Timestamp('2024-12-03T08:01:00.000000Z')),
                    ('t2-9'u, 't1-2'u, null, 'BAD'u, Timestamp('2024-12-03T09:01:00.000000Z'));

                UPSERT INTO `Root/tab5` (t5_id, t5_coll, t5_exec_dt) VALUES
                    ('k00'u, null,   Timestamp('2024-12-03T01:00:00.000000Z')),
                    ('k01'u, null,   Timestamp('2024-12-03T02:00:00.000000Z')),
                    ('k02'u, null,   Timestamp('2024-12-03T03:00:00.000000Z')),
                    ('k10'u, 'c00'u, Timestamp('2024-12-03T08:00:00.000000Z'));

                UPSERT INTO `Root/tab6` (t6_id, t6_coll, t6_link_type) VALUES
                    ('k00'u, 'c00'u, 'l00'u),
                    ('k01'u, 'c00'u, 'l00'u),
                    ('k02'u, 'c00'u, 'l00'u);
            )"));

            auto result = session.ExecuteDataQuery(upsertSql, TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {  // join with data column req.ext_ref in predicate
            const TString joinSql(Q1_(R"(
                $sys_date = DateTime("2024-12-03T00:00:00Z");
                $ival = DateTime::IntervalFromDays(1);
                SELECT req.id, doc.id, req.int_ref, doc.dst_ident, req.ext_ref
                    FROM `Root/tab1` VIEW ix_exec_dt AS doc
                    LEFT JOIN `Root/tab2` VIEW ix_int_ref req
                    ON doc.id = req.int_ref AND doc.dst_ident = req.ext_ref
                    WHERE doc.exec_dt >= $sys_date and doc.exec_dt <$sys_date + $ival
                    AND doc.id='t1-1'u;
            )"));

            auto result = session.ExecuteDataQuery(joinSql, TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            CompareYson(R"([
                [["t2-3"];"t1-1";["t1-1"];["id-2"];["id-2"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }

        {  // join with data column req.ext_ref in predicate
            const TString joinSql(Q1_(R"(
                $sys_date = DateTime("2024-12-03T00:00:00Z");
                $ival = DateTime::IntervalFromDays(1);
                SELECT doc.id, req.id, doc.dst_ident, req.ext_ref, req.good_sign
                    FROM `Root/tab3` VIEW ix_exec_dt AS doc
                    LEFT JOIN (
                        SELECT id, int_ref, ext_ref, good_sign
                        FROM `Root/tab4` VIEW ix_int_ref
                        WHERE good_sign='GOOD'u
                    ) AS req
                    ON doc.id = req.int_ref AND doc.dst_ident = req.ext_ref
                    WHERE doc.exec_dt >= $sys_date and doc.exec_dt <$sys_date + $ival
                    AND doc.id='t1-1'u;
            )"));

            auto result = session.ExecuteDataQuery(joinSql, TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            CompareYson(R"([
                ["t1-1";["t2-3"];["id-2"];["id-2"];["GOOD"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }

        {  // join with secondary index column in predicate
            const TString joinSql(Q1_(R"(
                SELECT * FROM (
                    SELECT t5.*, t6.*, t5owner.t5_id as owner_id
                    FROM (SELECT 'l00'u AS link_type) AS cond1
                    CROSS JOIN `Root/tab5` AS t5
                    LEFT JOIN `Root/tab6` VIEW ix_magic AS t6
                    ON t5.t5_id=t6.t6_id AND t6.t6_link_type=cond1.link_type
                    LEFT JOIN `Root/tab5` VIEW ix_ref_coll AS t5owner
                    ON t6.t6_coll=t5owner.t5_coll
                ) WHERE t5_exec_dt BETWEEN DateTime('2024-12-03T00:00:00Z') AND DateTime('2024-12-05T00:00:00Z')
                ORDER BY t5_id, t6_id, owner_id;
            )"));

            auto result = session.ExecuteDataQuery(joinSql, TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            CompareYson(R"([
                [["k10"];#;[1733187600000000u];"k00";["c00"];["k00"];["l00"]];
                [["k10"];#;[1733191200000000u];"k01";["c00"];["k01"];["l00"]];
                [["k10"];#;[1733194800000000u];"k02";["c00"];["k02"];["l00"]];
                [#;["c00"];[1733212800000000u];"k10";#;#;#]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(DirectAccessToIndexImplTable) {
        NKikimrConfig::TFeatureFlags featureFlags;
        featureFlags.SetEnableAccessToIndexImplTables(true);
        auto settings = TKikimrSettings().SetFeatureFlags(featureFlags);
        TKikimrRunner kikimr(settings);        
        auto db = kikimr.GetTableClient();
        kikimr.GetTestClient().GrantConnect("user@builtin");
        kikimr.GetTestServer().GetRuntime()->GetAppData().AdministrationAllowedSIDs.emplace_back("root@builtin");

        auto adminSession = kikimr.GetTableClient(NYdb::NTable::TClientSettings()
            .AuthToken("root@builtin")).CreateSession().GetValueSync().GetSession();

        CreateSampleTablesWithIndex(adminSession);
        const char *tablePath = "/Root/SecondaryKeys";
        const char *implTablePath = "/Root/SecondaryKeys/Index/indexImplTable";

        // a user which does not have any implicit permissions
        auto userClient = kikimr.GetTableClient(NYdb::NTable::TClientSettings()
            .AuthToken("user@builtin"));
        auto userSession = userClient.CreateSession().GetValueSync().GetSession();

        auto selectTableQuery = [&]() {
            return userSession.ExecuteDataQuery(Sprintf(R"(
                SELECT * FROM `%s`;
            )", tablePath), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
        };
        auto selectImplTableQuery = [&]() {
            return userSession.ExecuteDataQuery(Sprintf(R"(
                SELECT * FROM `%s`;
            )", implTablePath), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
        };
        auto upsertTableQuery = [&]() {
            return userSession.ExecuteDataQuery(Sprintf(R"(
                UPSERT INTO `%s` (Key, Fk, Value) VALUES
                    (9,    9,    "Payload9");
            )", tablePath), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
        };        
        auto upsertImplTableQuery = [&]() {
            return userSession.ExecuteDataQuery(Sprintf(R"(
                UPSERT INTO `%s` (Fk, Key) VALUES
                    (99, 99u);
            )", implTablePath), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
        };
        auto updateImplTableQuery = [&]() {
            return userSession.ExecuteDataQuery(Sprintf(R"(
                UPDATE `%s` ON (Fk, Key) VALUES (99, 99u);
            )", implTablePath), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
        };
        auto deleteImplTableQuery = [&]() {
            return userSession.ExecuteDataQuery(Sprintf(R"(
                DELETE FROM `%s` WHERE Fk = 99;
            )", implTablePath), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
        };
        auto bulkUpsertImplTable = [&]() {
            TValueBuilder builder;
            builder.BeginList();
            builder.AddListItem().BeginStruct()
                .AddMember("Key").Int32(99)
                .AddMember("Fk").Int32(99)
            .EndStruct();
            builder.EndList();
            return userClient.BulkUpsert(implTablePath, builder.Build()).ExtractValueSync();
        };

        // try accessing tables without permissions
        {
            auto result = selectTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SCHEME_ERROR, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "it does not exist or you do not have access permissions",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = selectImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SCHEME_ERROR, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "it does not exist or you do not have access permissions",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = upsertTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SCHEME_ERROR, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "it does not exist or you do not have access permissions",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = upsertImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SCHEME_ERROR, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "it does not exist or you do not have access permissions",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = updateImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SCHEME_ERROR, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "it does not exist or you do not have access permissions",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = deleteImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SCHEME_ERROR, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "it does not exist or you do not have access permissions",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = bulkUpsertImplTable();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::UNAUTHORIZED, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "Access denied for user@builtin with access UpdateRow to table",
                result.GetIssues().ToString()
            );
        }

        // grant necessary permission
        Grant(adminSession, "USE", tablePath, "user@builtin");

        // try accessing tables with permissions
        {
            auto result = selectTableQuery();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            auto result = selectImplTableQuery();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            auto result = upsertTableQuery();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            auto result = upsertImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "Writing to index implementation tables is not allowed",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = updateImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "Writing to index implementation tables is not allowed",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = deleteImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "Writing to index implementation tables is not allowed",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = bulkUpsertImplTable();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "Writing to index implementation tables is not allowed",
                result.GetIssues().ToString()
            );
        }

        // become superuser
        kikimr.GetTestServer().GetRuntime()->GetAppData().AdministrationAllowedSIDs.emplace_back("user@builtin");
        
        // accessing tables as superuser
        {
            auto result = selectTableQuery();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            auto result = selectImplTableQuery();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            auto result = upsertTableQuery();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            auto result = upsertImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "Writing to index implementation tables is not allowed",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = updateImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "Writing to index implementation tables is not allowed",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = deleteImplTableQuery();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "Writing to index implementation tables is not allowed",
                result.GetIssues().ToString()
            );
        }
        {
            auto result = bulkUpsertImplTable();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(),
                "Writing to index implementation tables is not allowed",
                result.GetIssues().ToString()
            );
        }
    }    
}

}
}
