#pragma once
#include <util/generic/yexception.h>

#include "processor.h"

namespace NKikimr::NBackup {

class TProcessorBase : public IProcessor {
public:
    TMaybe<TBuffer> Process(TBuffer&& data) override {
        Y_DEBUG_ABORT_UNLESS(!Finished);
        if (Finished) {
            ythrow yexception() << "Process call on finished stream";
        }
        if (!data.size()) {
            ythrow yexception() << "Process call on empty data";
        }

        return ProcessImpl(std::move(data));
    }

    TMaybe<TBuffer> Finish() override {
        Y_DEBUG_ABORT_UNLESS(!Finished);
        if (Finished) {
            ythrow yexception() << "Finish call on already finished stream";
        }

        return FinishImpl();
    }

protected:
    virtual TMaybe<TBuffer> ProcessImpl(TBuffer&& data) = 0;
    virtual TMaybe<TBuffer> FinishImpl() = 0;

protected:
    bool Finished = false;
};

} // namespace NKikimr::NBackup
