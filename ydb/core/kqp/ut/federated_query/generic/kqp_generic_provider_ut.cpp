#include "ch_recipe_ut_helpers.h"
#include "connector_recipe_ut_helpers.h"
#include "pg_recipe_ut_helpers.h"

#include <ydb/library/yql/providers/generic/connector/libcpp/client.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/stream/format.h>

#include <fmt/format.h>

using namespace NTestUtils;
using namespace fmt::literals;

Y_UNIT_TEST_SUITE(GenericProviderPushdownTest) {
    size_t TableNum = 0;

    TString GetTableName(TStringBuf name) {
        return TStringBuilder() << name << '_' << TableNum;
    }

    void FindStringInPgImpl(const TString& pgTypeName, int padWithSpaces = -1) {
        ++TableNum;
        pqxx::connection pgConnection = CreatePostgresqlConnection();

        // pg_table_find_string_test
        {
            pqxx::work work{pgConnection};
            const TString sql = fmt::format(
                R"sql(
                    CREATE TABLE {table_name} (
                        key {type_name} PRIMARY KEY,
                        value {type_name}
                    )
                )sql",
                "table_name"_a = GetTableName("pg_table_find_string_test"),
                "type_name"_a = pgTypeName
            );
            work.exec(sql);

            const TString insertData = fmt::format(
                R"sql(
                    INSERT INTO {table_name}
                        (key, value)
                    VALUES
                        ('usual string',                        'usual string'),
                        ('space ',                              'space '),
                        ('string with spaces at the end     ',    'string with spaces at the end     '),
                        ('    string with spaces on start',     '    string with spaces on start');
                )sql",
                "table_name"_a = GetTableName("pg_table_find_string_test")
            );
            work.exec(insertData);

            work.commit();
        }

        // we can find these strings without spaces
        {
            auto findByKey = [&](const std::string& key, const std::optional<std::string>& expectedKey, const std::optional<std::string>& expectedValue) {
                pqxx::work work{pgConnection};
                const TString sql = fmt::format(
                    R"sql(
                        SELECT * FROM {table_name}
                        WHERE key = $1;
                    )sql",
                    "table_name"_a = GetTableName("pg_table_find_string_test")
                );

                Cerr << "Find row in postgresql by key: [" << key << "]" << Endl;

                pqxx::result result = work.exec_params(sql, key);
                UNIT_ASSERT_VALUES_EQUAL_C(result.size(), expectedKey ? 1 : 0, (expectedKey ? "Not found" : "Found") << " key: [" << key << "]");
                if (expectedKey) {
                    TStringBuilder k, v;
                    if (padWithSpaces > 0) {
                        k << RightPad(*expectedKey, padWithSpaces);
                        v << RightPad(*expectedValue, padWithSpaces);
                    } else {
                        k << *expectedKey;
                        v << *expectedValue;
                    }
                    pqxx::row row = *result.begin();
                    UNIT_ASSERT_VALUES_EQUAL(row["key"].c_str(), k);
                    UNIT_ASSERT_VALUES_EQUAL(row["value"].c_str(), v);
                }
            };

            findByKey("usual string", "usual string", "usual string");
            findByKey("space", "space ", "space ");
            findByKey("string with spaces at the end", "string with spaces at the end     ", "string with spaces at the end     ");
            findByKey("string with spaces at the end                ", "string with spaces at the end     ", "string with spaces at the end     ");
            findByKey("string with spaces on start", std::nullopt, std::nullopt);
            findByKey("string  with  spaces  at  the  end", std::nullopt, std::nullopt);
            findByKey("    string with spaces on start   ", "    string with spaces on start", "    string with spaces on start");
        }

        return;


        std::shared_ptr<NKikimr::NKqp::TKikimrRunner> kikimr = MakeKikimrRunnerWithConnector();
        auto queryClient = kikimr->GetQueryClient();

        // external tables to pg/ch
        {
            const TString sql = fmt::format(
                R"sql(
                CREATE OBJECT pg_password_obj (TYPE SECRET) WITH (value="{pg_password}");
                CREATE EXTERNAL DATA SOURCE pg_data_source WITH (
                    SOURCE_TYPE="PostgreSQL",
                    LOCATION="{pg_host}:{pg_port}",
                    DATABASE_NAME="{pg_database}",
                    USE_TLS="FALSE",
                    AUTH_METHOD="BASIC",
                    PROTOCOL="NATIVE",
                    LOGIN="{pg_user}",
                    PASSWORD_SECRET_NAME="pg_password_obj"
                );

                CREATE OBJECT ch_password_obj (TYPE SECRET) WITH (value="{ch_password}");
                CREATE EXTERNAL DATA SOURCE ch_data_source WITH (
                    SOURCE_TYPE="ClickHouse",
                    LOCATION="{ch_host}:{ch_port}",
                    DATABASE_NAME="{ch_database}",
                    AUTH_METHOD="BASIC",
                    PROTOCOL="NATIVE",
                    LOGIN="{ch_user}",
                    PASSWORD_SECRET_NAME="ch_password_obj"
                );
                )sql",
                "pg_host"_a = GetPgHost(),
                "pg_port"_a = GetPgPort(),
                "pg_user"_a = GetPgUser(),
                "pg_password"_a = GetPgPassword(),
                "pg_database"_a = GetPgDatabase(),
                "ch_host"_a = GetChHost(),
                "ch_port"_a = GetChPort(),
                "ch_database"_a = GetChDatabase(),
                "ch_user"_a = GetChUser(),
                "ch_password"_a = GetChPassword());
            auto result = queryClient.ExecuteQuery(sql, NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        // join
        const TString sql = R"sql(
            SELECT pg.* FROM ch_data_source.ch_table_inner_join_test AS ch
            INNER JOIN pg_data_source.pg_table_inner_join_test AS pg
            ON ch.key = pg.key
            WHERE ch.key > 998
        )sql";

        auto result = queryClient.ExecuteQuery(sql, NYdb::NQuery::TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        // results
        auto resultSet = result.GetResultSetParser(0);
        UNIT_ASSERT_VALUES_EQUAL(resultSet.RowsCount(), 1);
        UNIT_ASSERT(resultSet.TryNextRow());

        const TMaybe<i32> key = resultSet.ColumnParser("key").GetOptionalInt32();
        UNIT_ASSERT(key);
        UNIT_ASSERT_VALUES_EQUAL(*key, 1000);

        const TMaybe<TString> name = resultSet.ColumnParser("name").GetOptionalUtf8();
        UNIT_ASSERT(name);
        UNIT_ASSERT_VALUES_EQUAL(name, "C");
    }

    Y_UNIT_TEST(FindStringInPgBpChar) {
        FindStringInPgImpl("BPCHAR");
    }

    Y_UNIT_TEST(FindStringInPgCharacter) {
        FindStringInPgImpl("CHARACTER(50)", 50);
    }
}
