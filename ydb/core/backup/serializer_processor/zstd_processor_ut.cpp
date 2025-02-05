#include "zstd_processor.h"

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NBackup {

Y_UNIT_TEST_SUITE(ZStdCompressionProcessorsTest) {
    Y_UNIT_TEST(CompressDecompress) {
        size_t parts = 100;
        TString input = "hello world! ";
        std::vector<IProcessor::TPtr> processors;
        processors.emplace_back(CreateZstdCompressingProcessor(22));
        processors.emplace_back(CreateZstdDecompressingProcessor(1024));

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
    }
}

} // namespace NKikimr::NBackup
