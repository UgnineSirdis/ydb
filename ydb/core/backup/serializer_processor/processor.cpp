#include "processor_base.h"

namespace NKikimr::NBackup {

class TDummyProcessor : public TProcessorBase {
protected:
    TMaybe<TBuffer> GetImpl() override {
        NextData();
        return std::move(CurrentData);
    }

    TMaybe<TBuffer> FinishImpl() override {
        return Nothing();
    }
};

class TChainProcessor : public TProcessorBase {
public:
    TChainProcessor(std::vector<IProcessor::TPtr> processors)
        : Processors(std::move(processors))
    {
        Y_DEBUG_ABORT_UNLESS(!Processors.empty());
    }

protected:
    void FeedImpl(TBuffer&& data) override {
        ResetFeed = true;
        Processors[0]->Feed(std::move(data));
    }

    TMaybe<TBuffer> GetImpl() override {
        if (ResetFeed) {
            ResetFeed = false;
            for (size_t i = 0; i < Processors.size() - 1; ++i) {
                while (TMaybe<TBuffer> data = Processors[i]->Get()) {
                    Processors[i + 1]->Feed(std::move(*data));
                }
            }
        }
        return Processors.back()->Get();
    }

    TMaybe<TBuffer> FinishImpl() override {
        for (size_t i = 0; i < Processors.size() - 1; ++i) {
            while (TMaybe<TBuffer> data = Processors[i]->Get()) {
                Processors[i + 1]->Feed(std::move(*data));
            }
            if (TMaybe<TBuffer> data = Processors[i]->Finish()) {
                Processors[i + 1]->Feed(std::move(*data));
            }
        }
        TMaybe<TBuffer> result;
        while (TMaybe<TBuffer> data = Processors.back()->Get()) {
            if (!result) {
                result = std::move(data);
            } else {
                result->Append(data->Data(), data->Size());
            }
        }
        if (TMaybe<TBuffer> data = Processors.back()->Finish()) {
            if (!result) {
                result = std::move(data);
            } else {
                result->Append(data->Data(), data->Size());
            }
        }
        return result;
    }

private:
    std::vector<IProcessor::TPtr> Processors;
    bool ResetFeed = false;
};

IProcessor::TPtr CreateDummyProcessor() {
    return MakeIntrusive<TDummyProcessor>();
}

IProcessor::TPtr CreateChainProcessor(std::vector<IProcessor::TPtr> processors) {
    return MakeIntrusive<TChainProcessor>(std::move(processors));
}

} // namespace NKikimr::NBackup
