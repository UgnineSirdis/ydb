#pragma once
#include <util/generic/buffer.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>
#include <util/generic/string.h>

#include <vector>

namespace NKikimr::NBackup {

// Streaming interface for processing or unprocessing backup data
// Examples:
// - compression/decompression
// - encryption/decryption
// - dummy
// - checksum calculation and verification
struct IProcessor : public TSimpleRefCount<IProcessor> {
    using TPtr = TIntrusivePtr<IProcessor>;

    virtual ~IProcessor() = default;

    // Takes new data for processing.
    virtual void Feed(TBuffer&& data) = 0;

    // Processes new data and returns current result part.
    // Current result part may be nothing.
    // Can throw error.
    virtual TMaybe<TBuffer> Get() = 0;

    // Processes last part, finishes stream.
    // Can throw error.
    virtual TMaybe<TBuffer> Finish() = 0;
};

// Common serializers

// Processor that does not do anything.
IProcessor::TPtr CreateDummyProcessor();

// Chain processor.
// Applies its subprocessors from begin to end and returns the result from the last one.
IProcessor::TPtr CreateChainProcessor(std::vector<IProcessor::TPtr> processors);

} // namespace NKikimr::NBackup
