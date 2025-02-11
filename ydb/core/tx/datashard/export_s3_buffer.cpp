#ifndef KIKIMR_DISABLE_S3_OPS

#include "export_s3_buffer.h"
#include "type_serialization.h"

#include <ydb/core/backup/common/checksum.h>
#include <ydb/core/backup/serializer_processor/processor.h>
#include <ydb/core/backup/serializer_processor/processor.h>
#include <ydb/core/backup/serializer_processor/checksum_processor.h>
#include <ydb/core/backup/serializer_processor/zstd_processor.h>
#include <ydb/core/tablet_flat/flat_row_state.h>
#include <yql/essentials/types/binary_json/read.h>
#include <ydb/public/lib/scheme_types/scheme_type_id.h>

#include <library/cpp/string_utils/quote/quote.h>

#include <util/datetime/base.h>
#include <util/generic/buffer.h>
#include <util/stream/buffer.h>


namespace NKikimr {
namespace NDataShard {

class TS3Buffer: public NExportScan::IBuffer {
    using TTagToColumn = IExport::TTableColumns;
    using TTagToIndex = THashMap<ui32, ui32>; // index in IScan::TRow

public:
    explicit TS3Buffer(const TTagToColumn& columns, ui64 rowsLimit, ui64 maxBytes, ui64 minBytes, bool enableChecksums, int compressionLevel);
    explicit TS3Buffer(const TTagToColumn& columns, ui64 rowsLimit, ui64 bytesLimit, bool enableChecksums);

    void ColumnsOrder(const TVector<ui32>& tags) override;
    bool Collect(const NTable::IScan::TRow& row) override;
    bool PrepareEvent(bool last, NExportScan::IBuffer::TStats& stats, THolder<IEventBase>& ev) override;
    void Clear() override;
    bool IsFilled() const override;
    TString GetError() const override;

protected:
    inline ui64 GetRowsLimit() const { return RowsLimit; }
    inline ui64 GetBytesLimit() const { return BytesLimit; }

    bool Collect(const NTable::IScan::TRow& row, IOutputStream& out);
    NBackup::IProcessor::TPtr CreateProcessor();

private:
    const TTagToColumn Columns;
    const ui64 RowsLimit;
    const ui64 BytesLimit;
    const ui64 MinBytes;
    const int CompressionLevel;

    TTagToIndex Indices;

protected:
    ui64 Rows;
    ui64 BytesRead;
    TBuffer Buffer;

    NBackup::IChecksum::TPtr Checksum;
    NBackup::IProcessor::TPtr Processor;

    TString ErrorString;
}; // TS3BufferRaw

TS3Buffer::TS3Buffer(const TTagToColumn& columns, ui64 rowsLimit, ui64 maxBytes, ui64 minBytes, bool enableChecksums, int compressionLevel)
    : Columns(columns)
    , RowsLimit(rowsLimit)
    , BytesLimit(maxBytes)
    , MinBytes(minBytes)
    , CompressionLevel(compressionLevel)
    , Rows(0)
    , BytesRead(0)
    , Checksum(enableChecksums ? NBackup::CreateChecksum() : nullptr)
    , Processor(CreateProcessor())
{
}

TS3Buffer::TS3Buffer(const TTagToColumn& columns, ui64 rowsLimit, ui64 bytesLimit, bool enableChecksums)
    : TS3Buffer(columns, rowsLimit, bytesLimit, 0, enableChecksums, -1)
{
}

NBackup::IProcessor::TPtr TS3Buffer::CreateProcessor() {
    std::vector<NBackup::IProcessor::TPtr> processors;
    if (Checksum) {
        processors.emplace_back(NBackup::CreateChecksumProcessor(Checksum));
    }

    if (CompressionLevel != -1) {
        processors.emplace_back(NBackup::CreateZstdCompressingProcessor(CompressionLevel));
    }

    if (processors.empty()) {
        return NBackup::CreateDummyProcessor();
    }
    if (processors.size() == 1) {
        return std::move(processors[0]);
    }
    return NBackup::CreateChainProcessor(std::move(processors));
}

void TS3Buffer::ColumnsOrder(const TVector<ui32>& tags) {
    Y_ABORT_UNLESS(tags.size() == Columns.size());

    Indices.clear();
    for (ui32 i = 0; i < tags.size(); ++i) {
        const ui32 tag = tags.at(i);
        auto it = Columns.find(tag);
        Y_ABORT_UNLESS(it != Columns.end());
        Y_ABORT_UNLESS(Indices.emplace(tag, i).second);
    }
}

bool TS3Buffer::Collect(const NTable::IScan::TRow& row, IOutputStream& out) {
    bool needsComma = false;
    for (const auto& [tag, column] : Columns) {
        auto it = Indices.find(tag);
        Y_ABORT_UNLESS(it != Indices.end());
        Y_ABORT_UNLESS(it->second < (*row).size());
        const auto& cell = (*row)[it->second];

        BytesRead += cell.Size();

        if (needsComma) {
            out << ",";
        } else {
            needsComma = true;
        }

        if (cell.IsNull()) {
            out << "null";
            continue;
        }

        bool serialized = true;
        switch (column.Type.GetTypeId()) {
        case NScheme::NTypeIds::Int32:
            serialized = cell.ToStream<i32>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Uint32:
            serialized = cell.ToStream<ui32>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Int64:
            serialized = cell.ToStream<i64>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Uint64:
            serialized = cell.ToStream<ui64>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Uint8:
        //case NScheme::NTypeIds::Byte:
            out << static_cast<ui32>(cell.AsValue<ui8>());
            break;
        case NScheme::NTypeIds::Int8:
            out << static_cast<i32>(cell.AsValue<i8>());
            break;
        case NScheme::NTypeIds::Int16:
            serialized = cell.ToStream<i16>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Uint16:
            serialized = cell.ToStream<ui16>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Bool:
            serialized = cell.ToStream<bool>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Double:
            serialized = cell.ToStream<double>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Float:
            serialized = cell.ToStream<float>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Date:
            out << TInstant::Days(cell.AsValue<ui16>());
            break;
        case NScheme::NTypeIds::Datetime:
            out << TInstant::Seconds(cell.AsValue<ui32>());
            break;
        case NScheme::NTypeIds::Timestamp:
            out << TInstant::MicroSeconds(cell.AsValue<ui64>());
            break;
        case NScheme::NTypeIds::Interval:
            serialized = cell.ToStream<i64>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Date32:
            serialized = cell.ToStream<i32>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Datetime64:
        case NScheme::NTypeIds::Timestamp64:
        case NScheme::NTypeIds::Interval64:
            serialized = cell.ToStream<i64>(out, ErrorString);
            break;
        case NScheme::NTypeIds::Decimal:
            serialized = DecimalToStream(cell.AsValue<std::pair<ui64, i64>>(), out, ErrorString, column.Type);
            break;
        case NScheme::NTypeIds::DyNumber:
            serialized = DyNumberToStream(cell.AsBuf(), out, ErrorString);
            break;
        case NScheme::NTypeIds::String:
        case NScheme::NTypeIds::String4k:
        case NScheme::NTypeIds::String2m:
        case NScheme::NTypeIds::Utf8:
        case NScheme::NTypeIds::Json:
        case NScheme::NTypeIds::Yson:
            out << '"' << CGIEscapeRet(cell.AsBuf()) << '"';
            break;
        case NScheme::NTypeIds::JsonDocument:
            out << '"' << CGIEscapeRet(NBinaryJson::SerializeToJson(cell.AsBuf())) << '"';
            break;
        case NScheme::NTypeIds::Pg:
            serialized = PgToStream(cell.AsBuf(), column.Type, out, ErrorString);
            break;
        case NScheme::NTypeIds::Uuid:
            serialized = UuidToStream(cell.AsValue<std::pair<ui64, ui64>>(), out, ErrorString);
            break;
        default:
            Y_ABORT("Unsupported type");
        }

        if (!serialized) {
            return false;
        }
    }

    out << "\n";
    ++Rows;

    return true;
}

bool TS3Buffer::Collect(const NTable::IScan::TRow& row) {
    TBufferOutput out(Buffer);
    ErrorString.clear();

    if (!Collect(row, out)) {
        return false;
    }

    return true;
}

bool TS3Buffer::PrepareEvent(bool last, NExportScan::IBuffer::TStats& stats, THolder<IEventBase>& ev) {
    try {
        stats.Rows = Rows;
        stats.BytesRead = BytesRead;

        Rows = 0;
        BytesRead = 0;
        if (!Buffer.Empty()) {
            Processor->Feed(std::move(Buffer));
            Buffer = TBuffer();
        }

        TBuffer outputBuffer;
        while (auto buffer = Processor->Get()) {
            outputBuffer.Append(buffer->Data(), buffer->Size());
        }
        if (last) {
            if (auto buffer = Processor->Finish()) {
                outputBuffer.Append(buffer->Data(), buffer->Size());
            }
        }

        if (!outputBuffer.Empty()) {
            stats.BytesSent = outputBuffer.Size();

            if (Checksum && last) {
                return new TEvExportScan::TEvBuffer<TBuffer>(std::move(outputBuffer), last, Checksum->Serialize());
            } else {
                return new TEvExportScan::TEvBuffer<TBuffer>(std::move(outputBuffer), last);
            }
        }
        return true;
    } catch (const std::exception& ex) {
        ErrorString = ex.what();
        return false;
    }
}

void TS3Buffer::Clear() {
    Rows = 0;
    BytesRead = 0;
    Buffer.Clear();
}

bool TS3Buffer::IsFilled() const {
    if (Buffer.Size() < MinBytes) {
        return false;
    }
    return Rows >= GetRowsLimit() || Buffer.Size() >= GetBytesLimit();
}

TString TS3Buffer::GetError() const {
    return ErrorString;
}

NExportScan::IBuffer* CreateS3ExportBufferRaw(
        const IExport::TTableColumns& columns, ui64 rowsLimit, ui64 bytesLimit, bool enableChecksums)
{
    return new TS3Buffer(columns, rowsLimit, bytesLimit, enableChecksums);
}

NExportScan::IBuffer* CreateS3ExportBufferZstd(int compressionLevel,
        const IExport::TTableColumns& columns, ui64 maxRows, ui64 maxBytes, ui64 minBytes, bool enableChecksums)
{
    return new TS3Buffer(columns, maxRows, maxBytes, minBytes, enableChecksums, compressionLevel);
}

} // NDataShard
} // NKikimr

#endif // KIKIMR_DISABLE_S3_OPS
