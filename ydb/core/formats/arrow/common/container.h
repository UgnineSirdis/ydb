#pragma once

#include <ydb/core/formats/arrow/accessor/abstract/accessor.h>

#include <ydb/library/accessor/accessor.h>
#include <ydb/library/conclusion/result.h>
#include <ydb/library/conclusion/status.h>
#include <ydb/library/formats/arrow/modifier/schema.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/table.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/type.h>
#include <util/string/builder.h>
#include <util/system/types.h>

namespace NKikimr::NArrow {

class IFieldsConstructor {
private:
    virtual std::shared_ptr<arrow::Scalar> DoGetDefaultColumnElementValue(const std::string& fieldName) const = 0;

public:
    TConclusion<std::shared_ptr<arrow::Scalar>> GetDefaultColumnElementValue(const std::shared_ptr<arrow::Field>& field, const bool force) const;
};

class TGeneralContainer {
private:
    std::optional<ui64> RecordsCount;
    YDB_READONLY_DEF(std::shared_ptr<NModifier::TSchema>, Schema);
    YDB_READONLY_DEF(std::vector<std::shared_ptr<NAccessor::IChunkedArray>>, Columns);
    void Initialize();

public:
    TGeneralContainer(const ui32 recordsCount);

    TGeneralContainer ApplyFilter(const TColumnFilter& filter) const;

    TGeneralContainer Slice(const ui32 offset, const ui32 count) const {
        std::vector<std::shared_ptr<NAccessor::IChunkedArray>> columns;
        for (auto&& i : Columns) {
            columns.emplace_back(i->ISlice(offset, count));
        }
        return TGeneralContainer(Schema->GetFields(), std::move(columns));
    }

    ui64 GetRawSizeVerified() const {
        ui64 result = 0;
        for (auto&& i : Columns) {
            result += i->GetRawSizeVerified();
        }
        return result;
    }

    ui32 GetRecordsCount() const {
        AFL_VERIFY(RecordsCount);
        return *RecordsCount;
    }

    NJson::TJsonValue DebugJson(const bool withData = false) const;

    TString DebugString(const bool withData = false) const {
        return DebugJson(withData).GetStringRobust();
    }

    [[nodiscard]] TConclusionStatus SyncSchemaTo(
        const std::shared_ptr<arrow::Schema>& schema, const IFieldsConstructor* defaultFieldsConstructor, const bool forceDefaults);

    bool HasColumn(const std::string& name) {
        return Schema->HasField(name);
    }

    ui64 num_columns() const {
        return Columns.size();
    }

    ui64 num_rows() const {
        AFL_VERIFY(RecordsCount);
        return *RecordsCount;
    }

    ui32 GetColumnsCount() const {
        return Columns.size();
    }

    const std::shared_ptr<NAccessor::IChunkedArray>& GetColumnVerified(const ui32 idx) const {
        AFL_VERIFY(idx < Columns.size());
        return Columns[idx];
    }

    class TTableConstructionContext: public NAccessor::TColumnConstructionContext {
    private:
        YDB_ACCESSOR_DEF(std::optional<std::set<std::string>>, ColumnNames);

    public:
        TTableConstructionContext() = default;
        TTableConstructionContext(std::set<std::string>&& columnNames)
            : ColumnNames(std::move(columnNames)) {
        }

        TTableConstructionContext(const std::set<std::string>& columnNames)
            : ColumnNames(columnNames) {
        }

        void SetColumnNames(const std::vector<TString>& names) {
            ColumnNames = std::set<std::string>(names.begin(), names.end());
        }
    };

    std::shared_ptr<arrow::Table> BuildTableVerified(const TTableConstructionContext& context = Default<TTableConstructionContext>()) const;
    std::shared_ptr<arrow::Table> BuildTableOptional(const TTableConstructionContext& context = Default<TTableConstructionContext>()) const;

    std::shared_ptr<TGeneralContainer> BuildEmptySame() const;

    [[nodiscard]] TConclusionStatus MergeColumnsStrictly(const TGeneralContainer& container);
    [[nodiscard]] TConclusionStatus AddField(const std::shared_ptr<arrow::Field>& f, const std::shared_ptr<NAccessor::IChunkedArray>& data);
    [[nodiscard]] TConclusionStatus AddField(const std::shared_ptr<arrow::Field>& f, const std::shared_ptr<arrow::Array>& data);

    [[nodiscard]] TConclusionStatus AddField(const std::shared_ptr<arrow::Field>& f, const std::shared_ptr<arrow::ChunkedArray>& data);

    void DeleteFieldsByIndex(const std::vector<ui32>& idxs);

    TGeneralContainer(const std::shared_ptr<arrow::Table>& table);
    TGeneralContainer(const std::shared_ptr<arrow::RecordBatch>& table);
    TGeneralContainer(const std::shared_ptr<arrow::Schema>& schema, std::vector<std::shared_ptr<NAccessor::IChunkedArray>>&& columns);
    TGeneralContainer(const std::shared_ptr<NModifier::TSchema>& schema, std::vector<std::shared_ptr<NAccessor::IChunkedArray>>&& columns);
    TGeneralContainer(
        const std::vector<std::shared_ptr<arrow::Field>>& fields, std::vector<std::shared_ptr<NAccessor::IChunkedArray>>&& columns);

    arrow::Status ValidateFull() const {
        return arrow::Status::OK();
    }

    std::shared_ptr<NAccessor::IChunkedArray> GetAccessorByNameOptional(const std::string& fieldId) const;
    std::shared_ptr<NAccessor::IChunkedArray> GetAccessorByNameVerified(const std::string& fieldId) const;
};

}   // namespace NKikimr::NArrow
