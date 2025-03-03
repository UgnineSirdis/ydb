#include "client_command_options.h"
#include "profile_manager.h"

#include <library/cpp/colorizer/colors.h>

#include <util/system/env.h>

namespace NYdb::NConsoleClient {

TClientCommandOptions::TClientCommandOptions(NLastGetopt::TOpts opts)
    : Opts(std::move(opts))
{
}

void TClientCommandOptions::SetTitle(const TString& title) {
    Opts.SetTitle(title);
}

TClientCommandOption& TClientCommandOptions::AddLongOption(const TString& name, const TString& help) {
    return AddClientOption(Opts.AddLongOption(name, help));
}

TClientCommandOption& TClientCommandOptions::AddLongOption(char c, const TString& name, const TString& help) {
    return AddClientOption(Opts.AddLongOption(c, name, help));
}

TClientCommandOption& TClientCommandOptions::AddClientOption(NLastGetopt::TOpt& opt) {
    return *ClientOpts.emplace_back(MakeIntrusive<TClientCommandOption>(opt, this));
}


TClientCommandOption::TClientCommandOption(NLastGetopt::TOpt& opt, TClientCommandOptions* clientOptions)
    : Opt(&opt)
    , ClientOptions(clientOptions)
    , Help(Opt->GetHelp())
{
}

TClientCommandOption& TClientCommandOption::RequiredArgument(const TString& title) {
    Opt->RequiredArgument(title);
    return *this;
}

TClientCommandOption& TClientCommandOption::StoreResult(TString* result) {
    ValueSetter = [result](const TString& v) {
        *result = v;
    };
    return SetHandler();
}

TClientCommandOption& TClientCommandOption::CustomProcessor(TCustomProcessor proc) {
    CustomProc = std::move(proc);
    return SetHandler();
}

TClientCommandOption& TClientCommandOption::Handler(THandler handler) {
    ValueHandler = std::move(handler);
    return SetHandler();
}

TClientCommandOption& TClientCommandOption::Validator(TValidator validator) {
    ValidatorHandler = std::move(validator);
    return SetHandler();
}

TClientCommandOption& TClientCommandOption::SetHandler() {
    if (!HandlerIsSet) {
        struct THandler : public NLastGetopt::IOptHandler {
            THandler(TClientCommandOption* self)
                : Self(self)
            {
            }

            void HandleOpt(const NLastGetopt::TOptsParser* parser) override {
                TString curVal(parser->CurValOrDef());
                Self->HandlerImpl(std::move(curVal));
            }

            TClientCommandOption* Self;
        };
        Opt->Handler(new THandler(this));
        HandlerIsSet = true;
    }
    return *this;
}

void TClientCommandOption::HandlerImpl(TString value) {
    if (CustomProc) {
        value = CustomProc(value);
    }
    if (ValueHandler) {
        ValueHandler(value);
    }
    if (ValueSetter) {
        ValueSetter(value);
    }
}

TClientCommandOption& TClientCommandOption::FileName(bool fileName) {
    IsFileName = fileName;
    return *this;
}

TClientCommandOption& TClientCommandOption::Env(const TString& envName, bool isFileName) {
    EnvInfo.emplace_back(
        TEnvInfo{
            .EnvName = envName,
            .IsFileName = isFileName,
        }
    );
    RebuildHelpMessage();
    return *this;
}

TClientCommandOption& TClientCommandOption::ProfileParam(const TString& profileParamName) {
    ProfileParamName = profileParamName;
    RebuildHelpMessage();
    return *this;
}

TClientCommandOption& TClientCommandOption::LogToConnectionParams(const TString& paramName) {
    ConnectionParamName = paramName;
    return *this;
}

void TClientCommandOption::RebuildHelpMessage() {
    TStringBuilder helpMessage;
    helpMessage << Help;
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);
    if (ClientOptions->HelpCommandVerbosiltyLevel >= 2) {
        helpMessage << Endl;
        helpMessage << "  Option search order:" << Endl;
        TStringBuf indent = "    ";
        size_t currentPoint = 1;
        helpMessage << indent << currentPoint++ << ". This option" << Endl;
        helpMessage << indent << currentPoint++ << ". Profile specified with --profile option" << Endl;
        for (const TEnvInfo& envInfo : EnvInfo) {
            helpMessage << indent << currentPoint++ << ". "
                << colors.BoldColor() << "\"" << envInfo.EnvName << "\"" << colors.OldColor()
                << " environment variable" << Endl;
        }
        helpMessage << indent << currentPoint++ << ". Active configuration profile";
    } else {
        if (!EnvInfo.empty()) {
            helpMessage << Endl;
            helpMessage << "Environment variable" << (EnvInfo.size() > 1 ? "s: " : ": ") << Endl;
            for (size_t i = 0; i < EnvInfo.size(); ++i) {
                if (i) {
                    helpMessage << ", ";
                }
                helpMessage << colors.BoldColor() << "\"" << EnvInfo[i].EnvName << "\"" << colors.OldColor();
            }
        }
    }
    Opt->Help(helpMessage);
}


TOptionsParseResult::TOptionsParseResult(const TClientCommandOptions* options, int argc, const char** argv)
    : ClientOptions(options)
    , ParseFromCommandLineResult(&options->GetOpts(), argc, argv)
{
    for (const auto& clientOption : ClientOptions->ClientOpts) {
        if (const auto* optResult = ParseFromCommandLineResult.FindOptParseResult(&clientOption->GetOpt())) {
            Opts.emplace_back(clientOption, optResult);
        }
    }
}

const TOptionParseResult* TOptionsParseResult::FindResult(const TClientCommandOption* opt) const {
    for (const TOptionParseResult& result : Opts) {
        if (result.GetOpt() == opt) {
            return &result;
        }
    }
    return nullptr;
}

const TOptionParseResult* TOptionsParseResult::FindResult(const TString& name) const {
    for (const auto& opt : Opts) {
        if (IsIn(opt.GetOpt()->GetOpt().GetLongNames(), name)) {
            return &opt;
        }
    }
    return nullptr;
}

const TOptionParseResult* TOptionsParseResult::FindResult(char name) const {
    for (const auto& opt : Opts) {
        if (IsIn(opt.GetOpt()->GetOpt().GetShortNames(), name)) {
            return &opt;
        }
    }
    return nullptr;
}

size_t TOptionsParseResult::GetFreeArgsPos() const {
    return ParseFromCommandLineResult.GetFreeArgsPos();
}

size_t TOptionsParseResult::GetFreeArgCount() const {
    return ParseFromCommandLineResult.GetFreeArgCount();
}

TVector<TString> TOptionsParseResult::GetFreeArgs() const {
    return ParseFromCommandLineResult.GetFreeArgs();
}

bool TOptionsParseResult::Has(const TString& name) const {
    return FindResult(name) != nullptr;
}

const TString& TOptionsParseResult::Get(const TString& name) const {
    if (const TOptionParseResult* result = FindResult(name)) {
        return result->Values().back();
    }
    throw yexception() << "No \"" << name << "\" option";
}

std::vector<TString> TOptionsParseResult::LogConnectionParams(const TConnectionParamsLogger& logger) {
    auto getProfileOpt = [](const TIntrusivePtr<TClientCommandOption>& opt, const std::shared_ptr<IProfile>& profile) {
        TString value;
        if (profile && opt->ProfileParamName && profile->Has(opt->ProfileParamName)) {
            value = profile->GetValue(opt->ProfileParamName).as<TString>();
        }
        return value;
    };
    std::vector<TString> messages;
    auto processValue = [&](const TIntrusivePtr<TClientCommandOption>& opt, const TString& value, const TString& sourceDescription, bool validate) {
        if (validate && opt->ValidatorHandler) {
            if (std::vector<TString> msgs = opt->ValidatorHandler(value); !msgs.empty()) {
                messages.insert(messages.end(), msgs.begin(), msgs.end());
            }
        }
        logger(opt->ConnectionParamName, value, sourceDescription);
    };
    for (const TOptionParseResult& result : Opts) {
        const TIntrusivePtr<TClientCommandOption>& opt = result.Opt;
        if (opt->ConnectionParamName) {
            // Log all available sources for current option
            for (EOptionValueSource src = result.ValueSource; ; src = static_cast<EOptionValueSource>(static_cast<int>(src) + 1)) {
                const bool validate = src != result.ValueSource; // We validate result in Validate() method
                switch (src) {
                case EOptionValueSource::Explicit: {
                    // already have parsed in that source
                    TStringBuilder txt;
                    txt << "explicit";
                    if (auto name = opt->GetOpt().GetLongNames()) {
                        txt << " --" << name[0];
                    } else if (auto shortNames = opt->GetOpt().GetShortNames()) {
                        txt << " -" << shortNames[0];
                    }
                    txt << " option";
                    for (const TString& value : result.OptValues) {
                        processValue(opt, value, txt, validate);
                    }
                    break;
                }
                case EOptionValueSource::ExplicitProfile: {
                    if (TString value = getProfileOpt(opt, ExplicitProfile)) {
                        TStringBuilder txt;
                        txt << "profile \"" << ExplicitProfile->GetName() << "\" from explicit --profile option";
                        processValue(opt, value, txt, validate);
                    }
                    break;
                }
                case EOptionValueSource::ActiveProfile: {
                    if (TString value = getProfileOpt(opt, ActiveProfile)) {
                        TStringBuilder txt;
                        txt << "active profile \"" << ActiveProfile->GetName() << "\"";
                        processValue(opt, value, txt, validate);
                    }
                    break;
                }
                case EOptionValueSource::EnvironmentVariable: {
                    for (const auto& envInfo : opt->EnvInfo) {
                        if (TString value = GetEnv(envInfo.EnvName)) {
                            TStringBuilder txt;
                            txt << envInfo.EnvName << " enviroment variable";
                            processValue(opt, value, txt, validate);
                        }
                    }
                    break;
                }
                case EOptionValueSource::DefaultValue:
                    if (opt->DefaultValue) {
                        processValue(opt, opt->DefaultValue, "default value", validate);
                    }
                    break;
                }
                if (src == EOptionValueSource::DefaultValue) {
                    break;
                }
            }
        }
    }
    return messages;
}

void TOptionsParseResult::ParseFromProfilesAndEnv(std::shared_ptr<IProfile> explicitProfile, std::shared_ptr<IProfile> activeProfile) {
    ExplicitProfile = std::move(explicitProfile);
    ActiveProfile = std::move(activeProfile);

    auto applyOption = [&](const TIntrusivePtr<TClientCommandOption>& clientOption, const TString& value, EOptionValueSource valueSource) {
        clientOption->HandlerImpl(value);
        Opts.emplace_back(clientOption, value, valueSource);
    };

    auto applyProfile = [](const TIntrusivePtr<TClientCommandOption>& clientOption, const std::shared_ptr<IProfile>& profile) {
        TString value;
        if (profile && clientOption->ProfileParamName && profile->Has(clientOption->ProfileParamName)) {
            value = profile->GetValue(clientOption->ProfileParamName).as<TString>();
        }
        return value;
    };

    for (const TIntrusivePtr<TClientCommandOption>& clientOption : ClientOptions->ClientOpts) {
        if (FindResult(clientOption.Get())) {
            continue;
        }

        if (TString value = applyProfile(clientOption, ExplicitProfile)) {
            applyOption(clientOption, value, EOptionValueSource::ExplicitProfile);
            continue;
        }

        bool envApplied = false;
        for (const auto& envInfo : clientOption->EnvInfo) {
            if (TString value = GetEnv(envInfo.EnvName)) {
                applyOption(clientOption, value, EOptionValueSource::EnvironmentVariable);
                envApplied = true;
                break;
            }
        }
        if (envApplied) {
            continue;
        }

        if (TString value = applyProfile(clientOption, ActiveProfile)) {
            applyOption(clientOption, value, EOptionValueSource::ActiveProfile);
            continue;
        }

        if (clientOption->DefaultValue) {
            applyOption(clientOption, clientOption->DefaultValue, EOptionValueSource::DefaultValue);
        }
    }
}

std::vector<TString> TOptionsParseResult::Validate() {
    std::vector<TString> messages;
    for (const TOptionParseResult& result : Opts) {
        if (result.Opt->ValidatorHandler) {
            for (const TString& value : result.Values()) {
                if (std::vector<TString> msgs = result.Opt->ValidatorHandler(value); !msgs.empty()) {
                    messages.insert(messages.end(), msgs.begin(), msgs.end());
                }
            }
        }
    }
    return messages;
}

} // namespace NYdb::NConsoleClient
