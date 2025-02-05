#include "checksum_processor.h"
#include "processor_base.h"

namespace NKikimr::NBackup {

namespace {

class TChecksumProcessor : public TProcessorBase {
public:
    explicit TChecksumProcessor(IChecksum::TPtr checksum)
        : Checksum(std::move(checksum))
    {
    }

    TMaybe<TBuffer> GetImpl() override {
        NextData();
        if (CurrentData) {
            Checksum->AddData(TStringBuf(CurrentData->Data(), CurrentData->Size()));
        }
        return std::move(CurrentData);
    }

    TMaybe<TBuffer> FinishImpl() override {
        return Nothing();
    }

private:
    IChecksum::TPtr Checksum;
};

} // anonymous

IProcessor::TPtr CreateChecksumProcessor(IChecksum::TPtr checksum) {
    return MakeIntrusive<TChecksumProcessor>(std::move(checksum));
}

} // namespace NKikimr::NBackup
