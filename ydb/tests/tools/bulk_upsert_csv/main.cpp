#include <library/cpp/getopt/last_getopt.h>

#include <util/generic/yexception.h>
#include <util/stream/file.h>
#include <util/stream/output.h>
#include <util/system/env.h>

#include <ydb/public/api/protos/ydb_formats.pb.h>
#include <ydb/public/sdk/cpp/client/ydb_driver/driver.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>

using namespace NLastGetopt;

int main(int argc, char** argv) {
    TString endpoint;
    TString database;
    TString table;
    TString csvPath;

    TOpts opts = TOpts::Default();
    opts.AddLongOption('e', "endpoint", "YDB endpoint").Required().RequiredArgument("HOST:PORT")
        .StoreResult(&endpoint);
    opts.AddLongOption('d', "database", "YDB database path").Required().RequiredArgument("PATH")
        .StoreResult(&database);
    opts.AddLongOption('t', "table", "YDB table path").Required().RequiredArgument("PATH")
        .StoreResult(&table);
    opts.AddLongOption('f', "csv", "Path to CSV file").Required().RequiredArgument("PATH")
        .StoreResult(&csvPath);

    TOptsParseResult parsedOpts(&opts, argc, argv);
    (void)parsedOpts;

    TString csvData;
    try {
        TFileInput input(csvPath);
        csvData = input.ReadAll();
    } catch (const yexception& e) {
        Cerr << "Failed to read CSV file '" << csvPath << "': " << e.what() << Endl;
        return 1;
    }

    NYdb::TDriverConfig driverConfig;
    driverConfig
        .SetEndpoint(endpoint)
        .SetDatabase(database)
        .SetAuthToken(GetEnv("YDB_TOKEN"));

    NYdb::TDriver driver(driverConfig);
    NYdb::NTable::TTableClient tableClient(driver);

    Ydb::Formats::CsvSettings csvSettings;
    csvSettings.set_header(true);
    csvSettings.set_delimiter(",");

    TString formatSettings;
    if (!csvSettings.SerializeToString(&formatSettings)) {
        Cerr << "Failed to serialize CSV format settings" << Endl;
        driver.Stop(true);
        return 1;
    }

    NYdb::NTable::TBulkUpsertSettings upsertSettings;
    upsertSettings.FormatSettings(formatSettings);

    auto result = tableClient.BulkUpsert(
        table,
        NYdb::NTable::EDataFormat::CSV,
        csvData,
        {},
        upsertSettings).GetValueSync();

    driver.Stop(true);

    if (!result.IsSuccess()) {
        Cerr << "BulkUpsert failed. Status: " << result.GetStatus()
             << ", issues: " << result.GetIssues().ToString() << Endl;
        return 2;
    }

    Cerr << "BulkUpsert completed successfully" << Endl;
    return 0;
}
