#pragma once

#include <library/cpp/getopt/last_getopt.h>

namespace NYdb::NConsoleClient {

class TClientCommandOption;
class TClientCommandOptions;
class TOptionsParseResult;
class IProfile;

enum class EOptionValueSource {
    Explicit,                // Command line
    ExplicitProfile,         // Profile that was set via command line
    EnvironmentVariable,     // Env variable
    ActiveProfile,           // Active profile
    DefaultValue,            // Default value for option
};

// Options for YDB CLI that support additional
// parsing features:
// - parsing from environment variables;
// - parsing from profile (for root options);
// - features for control that only one of alternative options is set;
// - features for control that alternative options or options that must be set together
//     are set from one source (command line, env or profile).
class TClientCommandOptions {
public:
    friend class TClientCommandOption;
    friend class TOptionsParseResult;

public:
    TClientCommandOptions(NLastGetopt::TOpts opts);

    // Current command title
    void SetTitle(const TString& title);

    TClientCommandOption& AddLongOption(const TString& name, const TString& help = "");
    TClientCommandOption& AddLongOption(char c, const TString& name, const TString& help = "");

    const NLastGetopt::TOpts& GetOpts() const {
        return Opts;
    }

    NLastGetopt::TOpts& GetOpts() {
        return Opts;
    }

    void SetHelpCommandVerbosiltyLevel(size_t level) {
        HelpCommandVerbosiltyLevel = level;
    }

private:
    TClientCommandOption& AddClientOption(NLastGetopt::TOpt& opt);

private:
    NLastGetopt::TOpts Opts;
    std::vector<TIntrusivePtr<TClientCommandOption>> ClientOpts;
    size_t HelpCommandVerbosiltyLevel = 1;
};

// YDB client command option
class TClientCommandOption : public TSimpleRefCount<TClientCommandOption> {
    friend class TOptionsParseResult;

public:
    using TCustomProcessor = std::function<TString(const TString&)>; // Modifies value
    using THandler = std::function<void(const TString&)>;
    using TValidator = std::function<std::vector<TString>(const TString&)>; // Returns list of messages with problems. Empty vector if OK

public:
    TClientCommandOption(NLastGetopt::TOpt& opt, TClientCommandOptions* clientOptions);

    TClientCommandOption& RequiredArgument(const TString& title = "");

    // Store result. If option is file name, stores the contents of file in result.
    TClientCommandOption& StoreResult(TString* result);

    template <class T>
    TClientCommandOption& StoreResult(T* result) {
        ValueSetter = [result](const TString& v) {
            *result = FromString<T>(v);
        };
        return SetHandler();
    }

    template <class T>
    TClientCommandOption& StoreResult(TMaybe<T>* result) {
        ValueSetter = [result](const TString& v) {
            *result = FromString<T>(v);
        };
        return SetHandler();
    }

    template <class T>
    TClientCommandOption& StoreResult(std::optional<T>* result) {
        ValueSetter = [result](const TString& v) {
            *result = FromString<T>(v);
        };
        return SetHandler();
    }

    TClientCommandOption& CustomProcessor(TCustomProcessor);
    TClientCommandOption& Handler(THandler);
    TClientCommandOption& Validator(TValidator);

    // YDB CLI specific options

    // This option means that its value must be read from file that is specified through command line.
    // Sets file content as option result (see StoreResult).
    TClientCommandOption& FileName(bool fileName = true);

    // Alternative env variable for option.
    // If isFileName is set, interprets current env variable as file name.
    TClientCommandOption& Env(const TString& envName, bool isFileName);

    // Parse option from profile
    TClientCommandOption& ProfileParam(const TString& profileParamName);

    // Log connection params at high verbosity level
    TClientCommandOption& LogToConnectionParams(const TString& paramName);

    NLastGetopt::TOpt& GetOpt() {
        return *Opt;
    }

    const NLastGetopt::TOpt& GetOpt() const {
        return *Opt;
    }

private:
    TClientCommandOption& SetHandler();
    void HandlerImpl(TString value);
    void RebuildHelpMessage();

private:
    struct TEnvInfo {
        TString EnvName;
        bool IsFileName = false;
    };

private:
    NLastGetopt::TOpt* Opt = nullptr;
    TClientCommandOptions* ClientOptions = nullptr;
    TString Help;
    TCustomProcessor CustomProc;
    TValidator ValidatorHandler;
    THandler ValueHandler;
    THandler ValueSetter;
    bool IsFileName = false;
    std::vector<TEnvInfo> EnvInfo;
    TString DefaultValue;
    TString ProfileParamName;
    TString ConnectionParamName;
    bool HandlerIsSet = false;
};

class TOptionParseResult {
    friend class TOptionsParseResult;

public:
    TOptionParseResult(const TOptionParseResult&) = default;
    TOptionParseResult(TOptionParseResult&&) = default;
    TOptionParseResult(TIntrusivePtr<TClientCommandOption> opt, const NLastGetopt::TOptParseResult* result)
        : Opt(std::move(opt))
        , ParseFromCommandLineResult(result)
        , ValueSource(EOptionValueSource::Explicit)
    {
        if (ParseFromCommandLineResult && !ParseFromCommandLineResult->Values().empty()) {
            OptValues.assign(ParseFromCommandLineResult->Values().begin(), ParseFromCommandLineResult->Values().end());
        }
    }

    TOptionParseResult(TIntrusivePtr<TClientCommandOption> opt, const TString& value, EOptionValueSource valueSource)
        : Opt(std::move(opt))
        , ValueSource(valueSource)
    {
        OptValues.emplace_back(std::move(value));
    }

    EOptionValueSource GetValueSource() const {
        return ValueSource;
    }

    const std::vector<TString>& Values() const {
        return OptValues;
    }

    size_t Count() const {
        return OptValues.size();
    }

    const TClientCommandOption* GetOpt() const {
        return Opt.Get();
    }

private:
    TIntrusivePtr<TClientCommandOption> Opt;
    const NLastGetopt::TOptParseResult* ParseFromCommandLineResult = nullptr;
    EOptionValueSource ValueSource;
    std::vector<TString> OptValues;
};

class TOptionsParseResult {
    friend class TClientCommandOptions;

public:
    using TConnectionParamsLogger = std::function<void(const TString& /*paramName*/, const TString& /*value*/, const TString& /*sourceText*/)>;

public:
    TOptionsParseResult(const TClientCommandOptions* options, int argc, const char** argv);

    void ParseFromProfilesAndEnv(std::shared_ptr<IProfile> explicitProfile, std::shared_ptr<IProfile> activeProfile);

    // Logging and validation
    std::vector<TString> Validate(); // Validate current values
    std::vector<TString> LogConnectionParams(const TConnectionParamsLogger& logger); // Check and log all connection params

    const TOptionParseResult* FindResult(const TClientCommandOption* opt) const;
    const TOptionParseResult* FindResult(const TString& name) const;
    const TOptionParseResult* FindResult(char name) const;

    size_t GetFreeArgsPos() const;

    size_t GetFreeArgCount() const;

    TVector<TString> GetFreeArgs() const;

    bool Has(const TString& name) const;

    const TString& Get(const TString& name) const;

private:
    const TClientCommandOptions* ClientOptions = nullptr;
    NLastGetopt::TOptsParseResult ParseFromCommandLineResult; // First parsing stage
    std::vector<TOptionParseResult> Opts;
    std::shared_ptr<IProfile> ExplicitProfile;
    std::shared_ptr<IProfile> ActiveProfile;
};

} // namespace NYdb::NConsoleClient
