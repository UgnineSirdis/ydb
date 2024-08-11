#include <ydb/public/sdk/cpp/client/ydb_discovery/discovery.h>
#include <ydb/public/sdk/cpp/client/ydb_driver/driver.h>
#include <ydb/public/sdk/cpp/client/ydb_types/credentials/oauth2_token_exchange/from_file.h>

#include <library/cpp/getopt/last_getopt.h>

#include <util/generic/yexception.h>
#include <util/stream/file.h>
#include <util/system/env.h>

#define REQ_OPT(name, value) Cerr << "    " #name ": \"" << value << "\"" << Endl;

struct TOptions {
    TString CaFile;
    TString ClientCert;
    TString ClientPrivateKey;
    bool YdbCredentialsFromEnv;
    TString Endpoint;
    TString Database;

    // Node registration
    TString Host;
    ui32 Port = 0;
    TString DomainPath;
    TString Address;

    TOptions(int argc, const char** argv) {
        InitFromCmdLine(argc, argv);
    }

    void InitFromCmdLine(int argc, const char** argv) {
        NLastGetopt::TOpts opts = NLastGetopt::TOpts::Default();

        opts.AddHelpOption('h');
        opts.AddLongOption('e', "endpoint", "YDB endpoint")
            .RequiredArgument("HOST:PORT")
            .StoreResult(&Endpoint);
        opts.AddLongOption('d', "database", "YDB database name")
            .RequiredArgument("PATH")
            .StoreResult(&Database);
        opts.AddLongOption("ca-file", "Path to a file containing the PEM encoding of the server root certificates for tls connections.\nIf this parameter is empty, the default roots will be used")
            .RequiredArgument("PATH")
            .StoreResult(&CaFile);
        opts.AddLongOption("client-cert", "Path to a file containing the PEM encoding of the client certificate")
            .RequiredArgument("PATH")
            .StoreResult(&ClientCert);
        opts.AddLongOption("client-private-key", "Path to a file containing the PEM encoding of the client private key")
            .RequiredArgument("PATH")
            .StoreResult(&ClientPrivateKey);
        opts.AddLongOption("cred-from-env", "Init Ydb credentials from environment variables (YDB_TOKEN, YDB_OAUTH2_KEY_FILE)")
            .SetFlag(&YdbCredentialsFromEnv);
        opts.AddLongOption("host", "Host param for discovery request")
            .RequiredArgument("HOST")
            .StoreResult(&Host);
        opts.AddLongOption("port", "Port param for discovery request")
            .RequiredArgument("PORT")
            .StoreResult(&Port);
        opts.AddLongOption("address", "Address param for discovery request")
            .RequiredArgument("ADDRESS")
            .StoreResult(&Address);
        opts.AddLongOption("domain-path", "Domain path for discovery request")
            .RequiredArgument("HOST:PORT")
            .StoreResult(&DomainPath);

        NLastGetopt::TOptsParseResult parseResult(&opts, argc, argv);

        if (ClientCert && !ClientPrivateKey || !ClientCert && ClientPrivateKey) {
            ythrow yexception() << "Both --client-cert and --client-private-key must be set or not set";
        }
    }

    std::shared_ptr<NYdb::ICredentialsProviderFactory> GetCredentials() const {
        if (!YdbCredentialsFromEnv) {
            return nullptr;
        }

        if (const TString token = GetEnv("YDB_TOKEN")) {
            REQ_OPT(CredentialsProviderFactory, "YDB_TOKEN");
            return NYdb::CreateOAuthCredentialsProviderFactory(token);
        }

        if (const TString oauth2KeyFile = GetEnv("YDB_OAUTH2_KEY_FILE")) {
            REQ_OPT(CredentialsProviderFactory, "YDB_OAUTH2_KEY_FILE");
            return NYdb::CreateOauth2TokenExchangeFileCredentialsProviderFactory(oauth2KeyFile);
        }

        ythrow yexception() << "Failed to init credentials from env: either YDB_TOKEN or YDB_OAUTH2_KEY_FILE must be set";
    }

    static TString GetFileContent(const TString& path) {
        return TFileInput(path).ReadAll();
    }

    NYdb::TDriver CreateYdbDriver() const {
        Cerr << "DriverOptions {" << Endl;
        NYdb::TDriverConfig conf;
        conf
            .SetDatabase(Database)
            .SetEndpoint(Endpoint);

        REQ_OPT(Database, Database);
        REQ_OPT(Endpoint, Endpoint);

        if (ClientCert) {
            conf
                .UseSecureConnection(GetFileContent(CaFile))
                .UseClientCertificate(GetFileContent(ClientCert), GetFileContent(ClientPrivateKey));
            REQ_OPT(UseSecureConnection, CaFile);
            REQ_OPT(ClientCert, ClientCert);
            REQ_OPT(ClientPrivateKey, ClientPrivateKey);
        } else if (CaFile) {
            conf.UseSecureConnection(GetFileContent(CaFile));
            REQ_OPT(UseSecureConnection, CaFile);
        }

        if (std::shared_ptr<NYdb::ICredentialsProviderFactory> creds = GetCredentials()) {
            conf.SetCredentialsProviderFactory(creds);
        }

        Cerr << "}" << Endl;
        Cerr << Endl;

        return NYdb::TDriver(conf);
    }
};

void RunWhoAmI(NYdb::NDiscovery::TDiscoveryClient& client) {
    NYdb::NDiscovery::TWhoAmIResult whoami = client.WhoAmI(NYdb::NDiscovery::TWhoAmISettings().WithGroups(true)).ExtractValueSync();
    Cerr << "== Who am I?" << Endl;
    if (whoami.IsSuccess()) {
        Cerr << whoami.GetUserName() << Endl;
        Cerr << Endl;
        Cerr << "Groups:" << Endl;
        for (const TString& group : whoami.GetGroups()) {
            Cerr << group << Endl;
        }
        Cerr << Endl;
    } else {
        Cerr << whoami.GetIssues().ToString() << Endl;
    }
}

void RunRegisterNode(NYdb::NDiscovery::TDiscoveryClient& client, const TOptions& opts) {
    NYdb::NDiscovery::TNodeRegistrationSettings settings;
    settings
        .Host(opts.Host)
        .Port(opts.Port)
        .Address(opts.Address);
    NYdb::NDiscovery::TNodeRegistrationResult registerResult = client.NodeRegistration(settings).ExtractValueSync();
    Cerr << "== Node registration" << Endl;
    Cerr << "Request {" << Endl;
    REQ_OPT(Host, opts.Host);
    REQ_OPT(Port, opts.Port);
    REQ_OPT(Address, opts.Address);
    Cerr << "}" << Endl;
    Cerr << Endl;
    if (registerResult.IsSuccess()) {
#define P(result, field) Cerr << #field ": " << result.Get ## field () << Endl;
#define PO(result, field) if (result.Has ## field ()) { P(result, field); }
#define N(node, field) Cerr << #field ": " << node.field << Endl;
#define L(location, field) if (location.field) { Cerr << #field ": " << *location.field << Endl; };

        P(registerResult, NodeId);
        P(registerResult, DomainPath);
        P(registerResult, Expire);
        PO(registerResult, ScopeTabletId);
        PO(registerResult, ScopePathId);
        PO(registerResult, NodeName);
        Cerr << Endl;

        for (size_t i = 0; i < registerResult.GetNodes().size(); ++i) {
            const auto& node = registerResult.GetNodes()[i];
            Cerr << "=== Node #" << node.NodeId << Endl;
            N(node, NodeId);
            N(node, Host);
            N(node, Port);
            N(node, ResolveHost);
            N(node, Address);
            //N(node, Location);
            N(node, Expire);
            if (node.Expire) {
                TInstant exp = TInstant::FromValue(node.Expire);
                TDuration diff = exp - TInstant::Now();
                Cerr << "Expire (human readable): " << exp << " (" << diff << ")" << Endl;
            }
            L(node.Location, DataCenterNum);
            L(node.Location, RoomNum);
            L(node.Location, RackNum);
            L(node.Location, BodyNum);
            L(node.Location, Body);
            L(node.Location, DataCenter);
            L(node.Location, Module);
            L(node.Location, Rack);
            L(node.Location, Unit);
            Cerr << Endl;
        }
    } else {
        Cerr << registerResult.GetIssues().ToString() << Endl;
    }
}

int main(int argc, const char** argv) {
    try {
        TOptions opts(argc, argv);
        NYdb::TDriver driver = opts.CreateYdbDriver();
        NYdb::NDiscovery::TDiscoveryClient client(driver);

        RunWhoAmI(client);
        RunRegisterNode(client, opts);
        return 0;
    } catch (const std::exception&) {
        Cerr << "Tool error:" << Endl;
        Cerr << CurrentExceptionMessage() << Endl;
        return 1;
    }
}
