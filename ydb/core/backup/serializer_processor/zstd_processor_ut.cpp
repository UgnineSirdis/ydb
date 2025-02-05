#include "zstd_processor.h"
#include "checksum_processor.h"

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NBackup {

Y_UNIT_TEST_SUITE(ZStdCompressionProcessorsTest) {
    Y_UNIT_TEST(CompressDecompress) {
        size_t parts = 100;
        TString input = "hello world! ";
        IChecksum::TPtr checksumCalcBefore = CreateChecksum();
        IChecksum::TPtr checksumCalcMiddle = CreateChecksum();
        IChecksum::TPtr checksumCalcAfter = CreateChecksum();
        std::vector<IProcessor::TPtr> processors;
        processors.emplace_back(CreateChecksumProcessor(checksumCalcBefore));
        processors.emplace_back(CreateZstdCompressingProcessor(22));
        processors.emplace_back(CreateChecksumProcessor(checksumCalcMiddle));
        processors.emplace_back(CreateZstdDecompressingProcessor(1024));
        processors.emplace_back(CreateChecksumProcessor(checksumCalcAfter));

        IProcessor::TPtr proc = CreateChainProcessor(std::move(processors));
        for (size_t i = 0; i < parts; ++i) {
            proc->Feed(TBuffer(input.data(), input.size()));
        }
        TBuffer result;
        while (TMaybe<TBuffer> data = proc->Get()) {
            result.Append(data->Data(), data->Size());
        }
        if (TMaybe<TBuffer> data = proc->Finish()) {
            result.Append(data->Data(), data->Size());
        }
        TString resultString;
        result.AsString(resultString);

        input *= parts;
        UNIT_ASSERT_STRINGS_EQUAL(input, resultString);

        TString checksumBefore = checksumCalcBefore->Serialize();
        TString checksumMiddle = checksumCalcMiddle->Serialize();
        TString checksumAfter = checksumCalcAfter->Serialize();
        UNIT_ASSERT_STRINGS_EQUAL(checksumBefore, checksumAfter);
        UNIT_ASSERT_STRINGS_UNEQUAL(checksumBefore, checksumMiddle);
    }
}

} // namespace NKikimr::NBackup
