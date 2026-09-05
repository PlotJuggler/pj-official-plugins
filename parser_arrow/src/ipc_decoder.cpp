#include "ipc_decoder.hpp"

// SDK 0.28 defines ArrowArrayStream under the C Data guard. Arrow 23 uses
// a separate stream guard; the identical ABI struct is already declared above.
#ifndef ARROW_C_STREAM_INTERFACE
#define ARROW_C_STREAM_INTERFACE
#endif
#include <arrow/c/bridge.h>
#include <arrow/compute/api.h>
#include <arrow/extension_type.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/record_batch.h>
#include <arrow/table.h>

#include <limits>

#include "arrow_error.hpp"

namespace pj::parser_arrow {
namespace {

// Unwrap encodings only after the record tap. Unsupported plotting types stay
// in the stream so the shaper can report them without losing recorded fields.
std::shared_ptr<arrow::DataType> storageType(const std::shared_ptr<arrow::DataType>& type) {
  switch (type->id()) {
    case arrow::Type::DICTIONARY:
      return storageType(std::static_pointer_cast<arrow::DictionaryType>(type)->value_type());
    case arrow::Type::EXTENSION:
      return storageType(std::static_pointer_cast<arrow::ExtensionType>(type)->storage_type());
    case arrow::Type::STRUCT: {
      auto fields = type->fields();
      bool changed = false;
      for (auto& field : fields) {
        auto storage = storageType(field->type());
        changed |= storage != field->type();
        field = field->WithType(storage);
      }
      return changed ? arrow::struct_(fields) : type;
    }
    case arrow::Type::LIST:
    case arrow::Type::LARGE_LIST:
    case arrow::Type::FIXED_SIZE_LIST:
    case arrow::Type::LIST_VIEW:
    case arrow::Type::LARGE_LIST_VIEW: {
      auto child = type->field(0)->WithType(storageType(type->field(0)->type()));
      if (type->id() == arrow::Type::LIST_VIEW) {
        return arrow::list(child);
      }
      if (type->id() == arrow::Type::LARGE_LIST_VIEW) {
        return arrow::large_list(child);
      }
      if (child->type() == type->field(0)->type()) {
        return type;
      }
      if (type->id() == arrow::Type::LIST) {
        return arrow::list(child);
      }
      if (type->id() == arrow::Type::LARGE_LIST) {
        return arrow::large_list(child);
      }
      return arrow::fixed_size_list(child, std::static_pointer_cast<arrow::FixedSizeListType>(type)->list_size());
    }
    default:
      return type;
  }
}

class StorageReader : public arrow::RecordBatchReader {
 public:
  explicit StorageReader(std::shared_ptr<arrow::RecordBatchReader> input) : input_(std::move(input)) {
    auto fields = input_->schema()->fields();
    for (auto& field : fields) {
      field = field->WithType(storageType(field->type()));
    }
    schema_ = arrow::schema(fields, input_->schema()->metadata());
  }

  std::shared_ptr<arrow::Schema> schema() const override {
    return schema_;
  }

  arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch>* out) override {
    ARROW_RETURN_NOT_OK(input_->ReadNext(out));
    if (!*out) {
      return arrow::Status::OK();
    }
    ARROW_RETURN_NOT_OK((*out)->ValidateFull());
    auto columns = (*out)->columns();
    for (int i = 0; i < (*out)->num_columns(); ++i) {
      if (!columns[i]->type()->Equals(schema_->field(i)->type())) {
        ARROW_ASSIGN_OR_RAISE(columns[i], arrow::compute::Cast(*columns[i], schema_->field(i)->type()));
      }
    }
    *out = arrow::RecordBatch::Make(schema_, (*out)->num_rows(), std::move(columns));
    return arrow::Status::OK();
  }

 private:
  std::shared_ptr<arrow::RecordBatchReader> input_;
  std::shared_ptr<arrow::Schema> schema_;
};

}  // namespace

PJ::Expected<PJ::sdk::ArrowStreamHolder> decodeIpcStream(PJ::Span<const uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int64_t>::max())) {
    return PJ::unexpected(parserError("empty or oversized Arrow IPC payload"));
  }
  auto buffer = std::make_shared<arrow::Buffer>(bytes.data(), static_cast<int64_t>(bytes.size()));
  auto input = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(buffer));
  if (!input.ok()) {
    return PJ::unexpected(parserError(input.status().ToString()));
  }
  PJ::sdk::ArrowStreamHolder stream;
  auto status = arrow::ExportRecordBatchReader(std::make_shared<StorageReader>(*input), stream.out());
  if (!status.ok()) {
    return PJ::unexpected(parserError(status.ToString()));
  }
  return stream;
}

PJ::Expected<std::shared_ptr<arrow::Table>> decodeIpcTable(PJ::Span<const uint8_t> bytes) {
  auto stream = decodeIpcStream(bytes);
  if (!stream) {
    return PJ::unexpected(stream.error());
  }
  auto reader = arrow::ImportRecordBatchReader(stream->get());
  if (!reader.ok()) {
    return PJ::unexpected(parserError(reader.status().ToString()));
  }
  auto table = (*reader)->ToTable();
  if (!table.ok()) {
    return PJ::unexpected(parserError(table.status().ToString()));
  }
  return *table;
}

}  // namespace pj::parser_arrow
