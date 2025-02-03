#include "zstd_processor.h"
#include "processor_base.h"

#include <util/generic/ptr.h>

#include <contrib/libs/zstd/include/zstd.h>

namespace NKikimr::NBackup {

namespace {

struct DestroyZCtx {
    static void Destroy(::ZSTD_CCtx* p) noexcept {
        ZSTD_freeCCtx(p);
    }

    static void Destroy(::ZSTD_DCtx* p) noexcept {
        ZSTD_freeDCtx(p);
    }
};

} // anonymous

class TZstdCompressingProcessor : public TProcessorBase {
public:
    explicit TZstdCompressingProcessor(int compressionLevel)
        : CompressionLevel(compressionLevel)
        , Context(ZSTD_createCCtx())
    {
        ZSTD_CCtx_reset(Context.Get(), ZSTD_reset_session_only);
        ZSTD_CCtx_refCDict(Context.Get(), NULL);
        ZSTD_CCtx_setParameter(Context.Get(), ZSTD_c_compressionLevel, CompressionLevel);
    }

    bool Compress(ZSTD_inBuffer* input, ZSTD_EndDirective endOp, TBuffer& result) {
        auto output = ZSTD_outBuffer{result.Data(), result.Capacity(), result.Size()};
        const size_t res = ZSTD_compressStream2(Context.Get(), &output, input, endOp);

        if (ZSTD_isError(res)) {
            ythrow yexception() << ZSTD_getErrorName(res);
        }

        if (res > 0) {
            result.Reserve(output.pos + res);
        }

        result.Proceed(output.pos);
        return res != 0;
    }

    TMaybe<TBuffer> ProcessImpl(TBuffer&& data) override {
        TMaybe<TBuffer> result;
        result.ConstructInPlace();
        result->Reserve(data.size());

        auto input = ZSTD_inBuffer{data.data(), data.size(), 0};
        while (input.pos < input.size) {
            Compress(&input, ZSTD_e_continue, *result);
        }
        return result;
    }

    TMaybe<TBuffer> FinishImpl() override {
        TMaybe<TBuffer> result;
        result.ConstructInPlace();

        auto input = ZSTD_inBuffer{NULL, 0, 0};
        while (!Compress(&input, ZSTD_e_end, *result));
        return result;
    }

private:
    const int CompressionLevel;
    THolder<::ZSTD_CCtx, DestroyZCtx> Context;
};

class TZstdDecompressingProcessor : public TProcessorBase {
public:
    TZstdDecompressingProcessor()
        : Context(ZSTD_createDCtx())
    {
        ZSTD_DCtx_reset(Context.Get(), ZSTD_reset_session_only);
        ZSTD_DCtx_refDDict(Context.Get(), NULL);
    }

    TMaybe<TBuffer> ProcessImpl(TBuffer&& data) override {
    }

    TMaybe<TBuffer> FinishImpl() override {
    }

private:
    THolder<::ZSTD_DCtx, DestroyZCtx> Context;
};

IProcessor::TPtr CreateZstdCompressingProcessor(int compressionLevel) {
    return MakeIntrusive<TZstdCompressingProcessor>(compressionLevel);
}

IProcessor::TPtr CreateZstdDecompressingProcessor() {
    return MakeIntrusive<TZstdDecompressingProcessor>();
}

} // namespace NKikimr::NBackup
