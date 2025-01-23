#pragma once
#include <util/generic/ptr.h>
#include <util/generic/string.h>

#include <vector>

namespace NKikimr::NBackup {

// Interfaces that implement RFC
// https://github.com/ydb-platform/ydb-rfc/blob/main/backup_encryption.md

// Streaming interface for packing or unpacking backup data
// Examples:
// - compression/decompression
// - encryption/decryption
// - dummy
// - checksum calculation and check
struct ISerializer : public TSimpleRefCount<ISerializer> {
    using TPtr = TIntrusivePtr<ISerializer>;

    virtual ~ISerializer() = default;

    // Packs/unpacks new data and returns current result part.
    // Current result part may be empty.
    // Can throw error.
    virtual TString Process(const TString& data) = 0;

    // Packs/unpacks last part, finishes stream.
    // Can throw error.
    virtual TString Finish() = 0;
};

// Common serializers

// Serializers that does not do anything.
ISerializer::TPtr CreateDummySerializer();

// Chain serializer.
// Applies its subserializers from begin to end and returns the result from the last one.
ISerializer::TPtr CreateChainSerializer(std::vector<ISerializer::TPtr> serializers);

} // namespace NKikimr::NBackup
