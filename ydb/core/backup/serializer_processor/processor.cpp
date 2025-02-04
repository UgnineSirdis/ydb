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
    TMaybe<TBuffer> GetImpl() override {
        return GetImpl(Processors.size() - 1);
    }

    TMaybe<TBuffer> FinishImpl() override {
        Y_DEBUG_ABORT_UNLESS(InputData.empty()); // All Get's were called
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
        if (TMaybe<TBuffer> data = Processors.back()->Get()) {
            if (!result) {
                result = std::move(data);
            } else {
                result->Append(data->Data(), data->Size());
            }
        }
        return result;
    }

    TMaybe<TBuffer> GetImpl(size_t i) {
        if (TMaybe<TBuffer> result = Processors[i]->Get()) {
            return result;
        }
        if (i) {
            if (TMaybe<TBuffer> prev = GetImpl(i - 1)) {
                Processors[i]->Feed(std::move(*prev));
            } else {
                return Nothing();
            }
        } else {
            if (NextData()) {
                Processors[0]->Feed(std::move(*CurrentData));
                CurrentData.Clear();
            } else {
                return Nothing();
            }
        }
        return Processors[i]->Get();
    }

private:
    std::vector<IProcessor::TPtr> Processors;
};

IProcessor::TPtr CreateDummyProcessor() {
    return MakeIntrusive<TDummyProcessor>();
}

IProcessor::TPtr CreateChainProcessor(std::vector<IProcessor::TPtr> processors) {
    return MakeIntrusive<TChainProcessor>(std::move(processors));
}

} // namespace NKikimr::NBackup
