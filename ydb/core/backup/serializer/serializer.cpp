#include "serializer_base.h"

namespace NKikimr::NBackup {

class TDummySerializer : public TSerializerBase {
protected:
    TString ProcessImpl(const TString& data) override {
        return data;
    }

    TString FinishImpl() override {
        return {};
    }
};

class TChainSerializer : public TSerializerBase {
public:
    TChainSerializer(std::vector<ISerializer::TPtr> serializers)
        : Serializers(std::move(serializers))
    {
    }

protected:
    TString ProcessImpl(const TString& data) override {
        const TString* input = &data;
        TString current;
        for (const ISerializer::TPtr& s : Serializers) {
            current = s->Process(*input);
            if (!current) {
                return {};
            }
            input = &current;
        }
        return current;
    }

    TString FinishImpl() override {
        TString current;
        for (const ISerializer::TPtr& s : Serializers) {
            if (current) {
                current = s->Process(current);
            }
            current += s->Finish();
        }
        return current;
    }

private:
    std::vector<ISerializer::TPtr> Serializers;
};

ISerializer::TPtr CreateDummySerializer() {
    return MakeIntrusive<TDummySerializer>();
}

ISerializer::TPtr CreateChainSerializer(std::vector<ISerializer::TPtr> serializers) {
    return MakeIntrusive<TChainSerializer>(std::move(serializers));
}

} // namespace NKikimr::NBackup
