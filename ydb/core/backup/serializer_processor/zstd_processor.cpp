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

class TZstdProcessorBase : public TProcessorBase {
protected:
    bool CheckInputData() { // returns true if there is data to compress
        if (!CurrentData || CurrentDataPos >= CurrentData->Size()) {
            CurrentDataPos = 0;
            NextData();
        }
        return CurrentData.Defined();
    }

    size_t CalcAllInputDataSize() const {
        size_t sum = 0;
        if (CurrentData) {
            sum += CurrentData->Size() - CurrentDataPos;
        }
        for (const TBuffer& d : InputData) {
            sum += d.Size();
        }
        return sum;
    }

protected:
    size_t CurrentDataPos = 0;
};

} // anonymous

class TZstdCompressingProcessor : public TZstdProcessorBase {
public:
    explicit TZstdCompressingProcessor(int compressionLevel)
        : CompressionLevel(compressionLevel)
        , Context(ZSTD_createCCtx())
    {
        ZSTD_CCtx_reset(Context.Get(), ZSTD_reset_session_only);
        ZSTD_CCtx_refCDict(Context.Get(), NULL);
        ZSTD_CCtx_setParameter(Context.Get(), ZSTD_c_compressionLevel, CompressionLevel);
    }

    bool Compress(ZSTD_inBuffer* input, ZSTD_EndDirective endOp, TBuffer& result, bool reserveMore = false) {
        auto output = ZSTD_outBuffer{result.Data(), result.Capacity(), result.Size()};
        const size_t res = ZSTD_compressStream2(Context.Get(), &output, input, endOp);
        CurrentDataPos = input->pos;

        if (ZSTD_isError(res)) {
            ythrow yexception() << ZSTD_getErrorName(res);
        }

        if (reserveMore && res > 0) {
            result.Reserve(output.pos + res);
        }

        result.Proceed(output.pos);
        return res != 0;
    }

    TMaybe<TBuffer> GetImpl() override {
        TMaybe<TBuffer> result;
        if (!CheckInputData()) {
            return result;
        }

        result.ConstructInPlace();
        result->Reserve(CurrentData->Size() - CurrentDataPos);

        auto input = ZSTD_inBuffer{CurrentData->Data(), CurrentData->Size(), CurrentDataPos};
        Compress(&input, ZSTD_e_continue, *result);
        return result;
    }

    TMaybe<TBuffer> FinishImpl() override {
        TMaybe<TBuffer> result;
        result.ConstructInPlace();
        result->Reserve(CalcAllInputDataSize());

        while (CheckInputData()) {
            auto input = ZSTD_inBuffer{CurrentData->Data(), CurrentData->Size(), CurrentDataPos};
            Compress(&input, ZSTD_e_continue, *result, true);
        }

        auto input = ZSTD_inBuffer{NULL, 0, 0};
        while (!Compress(&input, ZSTD_e_end, *result, true));
        return result;
    }

private:
    const int CompressionLevel;
    THolder<::ZSTD_CCtx, DestroyZCtx> Context;
};

class TZstdDecompressingProcessor : public TZstdProcessorBase {
public:
    explicit TZstdDecompressingProcessor(size_t bufferSize)
        : Context(ZSTD_createDCtx())
        , BufferSize(bufferSize)
    {
        ZSTD_DCtx_reset(Context.Get(), ZSTD_reset_session_only);
        ZSTD_DCtx_refDDict(Context.Get(), NULL);
    }

    TMaybe<TBuffer> GetImpl() override {
        TMaybe<TBuffer> result;
        if (!CheckInputData()) {
            return result;
        }

        result.ConstructInPlace();
        result->Reserve(BufferSize);

        auto input = ZSTD_inBuffer{CurrentData->Data(), CurrentData->Size(), CurrentDataPos};
        auto output = ZSTD_outBuffer{result->Data(), result->Capacity(), 0};
        const size_t res = ZSTD_decompressStream(Context.Get(), &output, &input);

        if (ZSTD_isError(res)) {
            ythrow yexception() << ZSTD_getErrorName(res);
        }

        CurrentDataPos = input.pos;
        result->Proceed(output.pos);
        return result;
    }

    TMaybe<TBuffer> FinishImpl() override {
        Y_DEBUG_ABORT_UNLESS(!CurrentData || CurrentDataPos >= CurrentData->Size());
        Y_DEBUG_ABORT_UNLESS(InputData.empty()); // All Get's were called
        return Nothing();
    }

private:
    THolder<::ZSTD_DCtx, DestroyZCtx> Context;
    const size_t BufferSize;
};

IProcessor::TPtr CreateZstdCompressingProcessor(int compressionLevel) {
    return MakeIntrusive<TZstdCompressingProcessor>(compressionLevel);
}

IProcessor::TPtr CreateZstdDecompressingProcessor(size_t bufferSize) {
    return MakeIntrusive<TZstdDecompressingProcessor>(bufferSize);
}

} // namespace NKikimr::NBackup
