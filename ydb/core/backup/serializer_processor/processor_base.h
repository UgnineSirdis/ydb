#pragma once
#include <util/generic/yexception.h>

#include "processor.h"

#include <deque>

namespace NKikimr::NBackup {

class TProcessorBase : public IProcessor {
public:
    void Feed(TBuffer&& data) override final {
        Y_DEBUG_ABORT_UNLESS(!Finished);
        if (Finished) {
            ythrow yexception() << "Feed finished stream";
        }
        if (!data.size()) {
            ythrow yexception() << "Feed empty data";
        }

        FeedImpl(std::move(data));
    }

    TMaybe<TBuffer> Get() override final {
        Y_DEBUG_ABORT_UNLESS(!Finished);
        if (Finished) {
            ythrow yexception() << "Get from finished stream";
        }

        return GetImpl();
    }

    TMaybe<TBuffer> Finish() override final {
        Y_DEBUG_ABORT_UNLESS(!Finished);
        if (Finished) {
            ythrow yexception() << "Finish call on already finished stream";
        }

        Finished = true;
        return FinishImpl();
    }

protected:
    virtual void FeedImpl(TBuffer&& data) {
        InputData.emplace_front(std::move(data));
    }

    virtual TMaybe<TBuffer> GetImpl() = 0;
    virtual TMaybe<TBuffer> FinishImpl() = 0;

    TMaybe<TBuffer>& NextData() {
        CurrentData.Clear();
        if (!InputData.empty()) {
            CurrentData.ConstructInPlace(std::move(InputData.back()));
            InputData.pop_back();
        }
        return CurrentData;
    }

protected:
    bool Finished = false;
    std::deque<TBuffer> InputData;
    TMaybe<TBuffer> CurrentData; // Current input data chunk
};

} // namespace NKikimr::NBackup
