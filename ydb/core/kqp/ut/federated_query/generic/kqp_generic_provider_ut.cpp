#include "ch_recipe_ut_helpers.h"
#include "connector_recipe_ut_helpers.h"
#include "pg_recipe_ut_helpers.h"

#include <ydb/library/yql/providers/generic/connector/libcpp/client.h>

#include <library/cpp/testing/unittest/registar.h>

#include <fmt/format.h>

using namespace NTestUtils;
using namespace fmt::literals;

Y_UNIT_TEST_SUITE(GenericProviderTest) {
    Y_UNIT_TEST(FindStringInPg) {
        pqxx::connection pgConnection = CreatePostgresqlConnection();

        // pg_table_find_string_test
        {
            pqxx::work work{pgConnection};
            const TString sql = R"sql(
                CREATE TABLE pg_table_find_string_test (
                    key TEXT PRIMARY KEY,
                    value TEXT
                )
            )sql";
            work.exec(sql);

            const TString insertData = R"sql(
                INSERT INTO pg_table_find_string_test
                    (key, value)
                VALUES
                    ('usual string',                        'usual string'),
                    ('string with spaces at the end \n \t   ',    'string with spaces at the end \n \t   '),
                    ('    string with spaces on start',     '    string with spaces on start');
            )sql";
            work.exec(insertData);

            work.commit();
        }

        // we can find these strings without spaces
        {
            auto findByKey = [&](const std::string& key, const std::string& expectedKey, const std::string& expectedValue) {
                pqxx::work work{pgConnection};
                const TString sql = R"sql(
                    SELECT * FROM pg_table_find_string_test
                    WHERE key = $1;
                )sql";

                Cerr << "Find row in postgresql by key: [" << key << "]" << Endl;

                pqxx::result result = work.exec_params(sql, key);
                UNIT_ASSERT_VALUES_EQUAL_C(result.size(), 1, "Not found key: [" << key << "]");
                pqxx::row row = *result.begin();
                UNIT_ASSERT_VALUES_EQUAL(row["key"].c_str(), expectedKey);
                UNIT_ASSERT_VALUES_EQUAL(row["value"].c_str(), expectedValue);
            };
            findByKey("usual string", "usual string", "usual string");
            findByKey("string with spaces at the end", "string with spaces at the end \n \t   ", "string with spaces at the end \n \t   ");
            findByKey("\n\nstring with spaces at the end\t", "string with spaces at the end \n \t   ", "string with spaces at the end \n \t   ");
            findByKey("string with spaces on start", "    string with spaces on start", "    string with spaces on start");
            findByKey("   string with spaces on start   ", "    string with spaces on start", "    string with spaces on start");
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
}
