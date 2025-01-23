#pragma once
#include <util/generic/yexception.h>

#include "serializer.h"

namespace NKikimr::NBackup {

class TSerializerBase : public ISerializer {
public:
    TString Process(const TString& data) override {
        Y_DEBUG_ABORT_UNLESS(!Finished);
        if (Finished) {
            ythrow yexception() << "Process call on finished stream";
        }
        if (data.empty()) {
            ythrow yexception() << "Process call on empty data";
        }

        return ProcessImpl(data);
    }

    TString Finish() override {
        Y_DEBUG_ABORT_UNLESS(!Finished);
        if (Finished) {
            ythrow yexception() << "Finish call on already finished stream";
        }

        return FinishImpl();
    }

protected:
    virtual TString ProcessImpl(const TString& data) = 0;
    virtual TString FinishImpl() = 0;

protected:
    bool Finished = false;
};

} // namespace NKikimr::NBackup
