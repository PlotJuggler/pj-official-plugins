#pragma once

#include <nanoarrow/nanoarrow.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/sdk/arrow.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "test_utils.hpp"

namespace pj::parser_arrow::test {

/// One Arrow stream consumed by ArrowWriteHostFake.
struct RecordedArrowStream {
  /// Child names from the stream schema, in schema order.
  std::vector<std::string> schema_names;
  /// Child formats from the stream schema, in schema order.
  std::vector<std::string> schema_formats;
  /// Row count of each record batch, in stream order.
  std::vector<int64_t> batch_row_counts;
  /// Int64 values pulled from the selected timestamp column.
  std::vector<int64_t> timestamp_values;
  /// Timestamp column selected by the parser.
  std::string timestamp_column;
};

/// Arrow-capable parser write host used by parser DSO integration tests.
class ArrowWriteHostFake {
 public:
  /// Build a full-size parser write host whose context points at this fake.
  [[nodiscard]] PJ_parser_write_host_t makeHost() noexcept {
    static const PJ_parser_write_host_vtable_t vtable = [] {
      PJ_parser_write_host_vtable_t table{};
      table.abi_version = PJ_PLUGIN_DATA_API_VERSION;
      table.struct_size = sizeof(PJ_parser_write_host_vtable_t);
      table.ensure_field = &ArrowWriteHostFake::trampolineEnsureField;
      table.append_record = &ArrowWriteHostFake::trampolineAppendRecord;
      table.append_bound_record = &ArrowWriteHostFake::trampolineAppendBoundRecord;
      table.append_arrow_stream = &ArrowWriteHostFake::trampolineAppendArrowStream;
      return table;
    }();
    return PJ_parser_write_host_t{.ctx = this, .vtable = &vtable};
  }

  /// Reject the next Arrow stream without taking ownership of it.
  void failNext() noexcept {
    fail_next_ = true;
  }

  /// Return every successfully consumed Arrow stream.
  [[nodiscard]] const std::vector<RecordedArrowStream>& streams() const noexcept {
    return streams_;
  }

  /// Return the number of ensure_field calls.
  [[nodiscard]] uint64_t ensureFieldCalls() const noexcept {
    return ensure_field_calls_;
  }

  /// Return the number of append_record calls.
  [[nodiscard]] uint64_t appendRecordCalls() const noexcept {
    return append_record_calls_;
  }

  /// Return the number of append_bound_record calls.
  [[nodiscard]] uint64_t appendBoundRecordCalls() const noexcept {
    return append_bound_record_calls_;
  }

  /// Return the number of append_arrow_stream calls, including failures.
  [[nodiscard]] uint64_t appendArrowStreamCalls() const noexcept {
    return append_arrow_stream_calls_;
  }

 private:
  /// Copy a C ABI string view into owned storage.
  [[nodiscard]] static std::string copyString(PJ_string_view_t value) {
    if (value.data == nullptr) {
      return {};
    }
    return std::string(value.data, static_cast<std::size_t>(value.size));
  }

  /// Fill a deterministic test-host error and return false.
  static bool fail(PJ_error_t* error, std::string_view message) noexcept {
    PJ::sdk::fillError(error, 1, "parser_arrow_test", message);
    return false;
  }

  /// Count an ensure_field call and return a stable dummy field handle.
  static bool trampolineEnsureField(
      void* context, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* output_field, PJ_error_t*) noexcept {
    auto* self = static_cast<ArrowWriteHostFake*>(context);
    ++self->ensure_field_calls_;
    *output_field = PJ_field_handle_t{PJ_topic_handle_t{1}, 1};
    return true;
  }

  /// Count an append_record call and accept it.
  static bool trampolineAppendRecord(
      void* context, int64_t, const PJ_named_field_value_t*, uint64_t, PJ_error_t*) noexcept {
    ++static_cast<ArrowWriteHostFake*>(context)->append_record_calls_;
    return true;
  }

  /// Count an append_bound_record call and accept it.
  static bool trampolineAppendBoundRecord(
      void* context, int64_t, const PJ_bound_field_value_t*, uint64_t, PJ_error_t*) noexcept {
    ++static_cast<ArrowWriteHostFake*>(context)->append_bound_record_calls_;
    return true;
  }

  /// Pull and record a complete Arrow stream, taking ownership only on success.
  static bool trampolineAppendArrowStream(
      void* context, ArrowArrayStream* stream, PJ_string_view_t timestamp_column, PJ_error_t* error) noexcept {
    auto* self = static_cast<ArrowWriteHostFake*>(context);
    ++self->append_arrow_stream_calls_;
    if (self->fail_next_) {
      self->fail_next_ = false;
      return fail(error, "configured Arrow stream rejection");
    }

    try {
      if (stream == nullptr || stream->release == nullptr || stream->get_schema == nullptr ||
          stream->get_next == nullptr) {
        return fail(error, "invalid Arrow stream");
      }

      PJ::sdk::ArrowSchemaHolder schema;
      const int schema_result = stream->get_schema(stream, schema.out());
      if (schema_result != NANOARROW_OK) {
        return fail(error, streamError(stream, "failed to pull Arrow schema"));
      }

      RecordedArrowStream recorded;
      recorded.timestamp_column = copyString(timestamp_column);
      int64_t timestamp_index = -1;
      for (int64_t child_index = 0; child_index < schema.get()->n_children; ++child_index) {
        const ArrowSchema* child = schema.get()->children[child_index];
        const std::string name = child != nullptr && child->name != nullptr ? child->name : "";
        recorded.schema_names.push_back(name);
        recorded.schema_formats.emplace_back(child != nullptr && child->format != nullptr ? child->format : "");
        if (name == recorded.timestamp_column) {
          timestamp_index = child_index;
        }
      }
      if (timestamp_index < 0) {
        return fail(error, "timestamp column is absent from Arrow schema");
      }

      while (true) {
        PJ::sdk::ArrowArrayHolder batch;
        const int batch_result = stream->get_next(stream, batch.out());
        if (batch_result != NANOARROW_OK) {
          return fail(error, streamError(stream, "failed to pull Arrow record batch"));
        }
        if (!batch.valid()) {
          break;
        }

        recorded.batch_row_counts.push_back(batch.get()->length);
        auto batch_view = bindArrayView(schema.get(), batch.get());
        for (int64_t row = 0; row < batch.get()->length; ++row) {
          recorded.timestamp_values.push_back(
              ArrowArrayViewGetIntUnsafe(batch_view.get()->children[timestamp_index], row));
        }
      }

      self->streams_.push_back(std::move(recorded));
      stream->release(stream);
      return true;
    } catch (const std::exception& exception) {
      return fail(error, exception.what());
    } catch (...) {
      return fail(error, "unknown Arrow write host failure");
    }
  }

  /// Return a stream diagnostic with a deterministic fallback.
  [[nodiscard]] static std::string_view streamError(ArrowArrayStream* stream, std::string_view fallback) noexcept {
    const char* message = ArrowArrayStreamGetLastError(stream);
    return message != nullptr && message[0] != '\0' ? std::string_view(message) : fallback;
  }

  bool fail_next_ = false;
  uint64_t ensure_field_calls_ = 0;
  uint64_t append_record_calls_ = 0;
  uint64_t append_bound_record_calls_ = 0;
  uint64_t append_arrow_stream_calls_ = 0;
  std::vector<RecordedArrowStream> streams_;
};

}  // namespace pj::parser_arrow::test
