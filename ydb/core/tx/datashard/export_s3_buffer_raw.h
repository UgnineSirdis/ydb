#pragma once

#ifndef KIKIMR_DISABLE_S3_OPS

#include "export_s3_buffer.h"

#include <ydb/core/backup/common/checksum.h>
#include <ydb/core/backup/serializer_processor/processor.h>

#include <util/generic/buffer.h>

namespace NKikimr {
namespace NDataShard {

class TS3BufferRaw: public NExportScan::IBuffer {
    using TTagToColumn = IExport::TTableColumns;
    using TTagToIndex = THashMap<ui32, ui32>; // index in IScan::TRow

public:
    explicit TS3BufferRaw(const TTagToColumn& columns, ui64 rowsLimit, ui64 bytesLimit, bool enableChecksums);

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

    TTagToIndex Indices;

protected:
    ui64 Rows;
    ui64 BytesRead;
    TBuffer Buffer;

    NBackup::IChecksum::TPtr Checksum;
    NBackup::IProcessor::TPtr Processor;

    TString ErrorString;
}; // TS3BufferRaw

} // NDataShard
} // NKikimr

#endif // KIKIMR_DISABLE_S3_OPS
