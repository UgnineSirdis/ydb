#include "processor_base.h"

namespace NKikimr::NBackup {

class TDummyProcessor : public TProcessorBase {
protected:
    TMaybe<TBuffer> ProcessImpl(TBuffer&& data) override {
        return std::move(data);
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
    }

protected:
    TMaybe<TBuffer> ProcessImpl(TBuffer&& data) override {
        TBuffer* input = &data;
        TMaybe<TBuffer> current;
        for (const IProcessor::TPtr& s : Processors) {
            current = s->Process(std::move(*input));
            if (!current || !current->size()) {
                return Nothing();
            }
            input = current.Get();
        }
        return current;
    }

    TMaybe<TBuffer> FinishImpl() override {
        TMaybe<TBuffer> current;
        for (const IProcessor::TPtr& s : Processors) {
            if (current && current->size()) {
                current = s->Process(std::move(*current));
            }
            if (TMaybe<TBuffer> f = s->Finish(); f && f->size()) {
                if (current && current->size()) {
                    current->Append(f->data(), f->size());
                } else {
                    current = std::move(f);
                }
            }
        }
        return current;
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
