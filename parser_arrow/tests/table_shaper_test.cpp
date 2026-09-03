#include "table_shaper.hpp"

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/sdk/arrow.hpp"
#include "test_utils.hpp"
#include "type_table.hpp"

#ifndef PJ_ARROW_TEST_DATA_DIR
#error "PJ_ARROW_TEST_DATA_DIR must be defined"
#endif

namespace pj::parser_arrow::test {

/// Invoke the production scalar-copy dispatch while keeping its shaping state private to table_shaper.cpp.
[[nodiscard]] int appendCastedValueForTesting(
    ArrowArray* output, const ArrowArrayView* input, int64_t row, const ArrowSchema* logical_schema);

/// Exercise the production seconds-to-nanoseconds conversion without depending on an Arrow storage type's precision.
[[nodiscard]] bool floatingSecondsToNanosecondsForTesting(double seconds, int64_t* output) noexcept;

/// Query the production scalar-copy predicate so the contract test cannot validate the type table against itself.
[[nodiscard]] bool supportsScalarCopyForTesting(ArrowType type) noexcept;

}  // namespace pj::parser_arrow::test

namespace pj::parser_arrow {
namespace {

using test::decodeFixture;
using test::readBatch;
using test::readSchema;

/// Construct a flat struct schema with the requested child names and types.
[[nodiscard]] PJ::sdk::ArrowSchemaHolder makeSchema(const std::vector<std::pair<std::string, ArrowType>>& fields) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  if (ArrowSchemaSetTypeStruct(schema.get(), static_cast<int64_t>(fields.size())) != NANOARROW_OK) {
    throw std::runtime_error("ArrowSchemaSetTypeStruct failed");
  }
  for (std::size_t index = 0; index < fields.size(); ++index) {
    auto* child = schema.get()->children[index];
    if (ArrowSchemaSetType(child, fields[index].second) != NANOARROW_OK ||
        ArrowSchemaSetName(child, fields[index].first.c_str()) != NANOARROW_OK) {
      throw std::runtime_error("child schema initialization failed");
    }
  }
  return schema;
}

/// Wrap a schema in an empty basic stream for observable shapeStream assertions.
[[nodiscard]] PJ::sdk::ArrowStreamHolder emptyStream(ArrowSchema* schema) {
  ArrowArrayStream stream{};
  if (ArrowBasicArrayStreamInit(&stream, schema, 0) != NANOARROW_OK) {
    throw std::runtime_error("ArrowBasicArrayStreamInit failed");
  }
  return PJ::sdk::ArrowStreamHolder(stream);
}

/// Wrap one owned batch in a basic stream using the supplied schema.
[[nodiscard]] PJ::sdk::ArrowStreamHolder oneBatchStream(ArrowSchema* schema, ArrowArray* batch) {
  ArrowArrayStream stream{};
  if (ArrowBasicArrayStreamInit(&stream, schema, 1) != NANOARROW_OK) {
    throw std::runtime_error("ArrowBasicArrayStreamInit failed");
  }
  ArrowBasicArrayStreamSetArray(&stream, 0, batch);
  return PJ::sdk::ArrowStreamHolder(stream);
}

/// Initialize one of the logical scalar schemas accepted by supportsScalarCopy, including parameterized types.
[[nodiscard]] PJ::sdk::ArrowSchemaHolder makeCopyableScalarSchema(ArrowType type) {
  PJ::sdk::ArrowSchemaHolder schema;
  int result = NANOARROW_OK;
  switch (type) {
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
      ArrowSchemaInit(schema.out());
      result = ArrowSchemaSetTypeFixedSize(schema.get(), type, 1);
      break;
    case NANOARROW_TYPE_TIME32:
      ArrowSchemaInit(schema.out());
      result = ArrowSchemaSetTypeDateTime(schema.get(), type, NANOARROW_TIME_UNIT_SECOND, nullptr);
      break;
    case NANOARROW_TYPE_TIME64:
    case NANOARROW_TYPE_DURATION:
    case NANOARROW_TYPE_TIMESTAMP:
      ArrowSchemaInit(schema.out());
      result = ArrowSchemaSetTypeDateTime(schema.get(), type, NANOARROW_TIME_UNIT_NANO, nullptr);
      break;
    case NANOARROW_TYPE_DECIMAL32:
    case NANOARROW_TYPE_DECIMAL64:
    case NANOARROW_TYPE_DECIMAL128:
    case NANOARROW_TYPE_DECIMAL256:
      ArrowSchemaInit(schema.out());
      result = ArrowSchemaSetTypeDecimal(schema.get(), type, 8, 3);
      break;
    default:
      result = ArrowSchemaInitFromType(schema.out(), type);
      break;
  }
  if (result != NANOARROW_OK) {
    throw std::runtime_error(std::string("scalar schema initialization failed for ") + ArrowTypeString(type));
  }
  return schema;
}

/// Append a representative value using the logical type's nanoarrow storage representation.
[[nodiscard]] int appendCopyableScalar(ArrowArray* array, ArrowType type) {
  switch (type) {
    case NANOARROW_TYPE_NA:
      return ArrowArrayAppendNull(array, 1);
    case NANOARROW_TYPE_BOOL:
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_DATE32:
    case NANOARROW_TYPE_DATE64:
    case NANOARROW_TYPE_TIME32:
    case NANOARROW_TYPE_TIME64:
    case NANOARROW_TYPE_DURATION:
    case NANOARROW_TYPE_TIMESTAMP:
      return ArrowArrayAppendInt(array, 1);
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
      return ArrowArrayAppendUInt(array, 1);
    case NANOARROW_TYPE_HALF_FLOAT:
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
      return ArrowArrayAppendDouble(array, 1.0);
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
    case NANOARROW_TYPE_STRING_VIEW:
    case NANOARROW_TYPE_BINARY_VIEW: {
      ArrowBufferView bytes{};
      bytes.data.as_char = "x";
      bytes.size_bytes = 1;
      return ArrowArrayAppendBytes(array, bytes);
    }
    case NANOARROW_TYPE_INTERVAL_MONTHS:
    case NANOARROW_TYPE_INTERVAL_DAY_TIME:
    case NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO: {
      ArrowInterval interval{};
      ArrowIntervalInit(&interval, type);
      interval.months = 1;
      interval.days = 2;
      interval.ms = 3;
      interval.ns = 4;
      return ArrowArrayAppendInterval(array, &interval);
    }
    case NANOARROW_TYPE_DECIMAL32:
    case NANOARROW_TYPE_DECIMAL64:
    case NANOARROW_TYPE_DECIMAL128:
    case NANOARROW_TYPE_DECIMAL256: {
      int32_t bitwidth = 256;
      if (type == NANOARROW_TYPE_DECIMAL32) {
        bitwidth = 32;
      } else if (type == NANOARROW_TYPE_DECIMAL64) {
        bitwidth = 64;
      } else if (type == NANOARROW_TYPE_DECIMAL128) {
        bitwidth = 128;
      }
      ArrowDecimal decimal{};
      ArrowDecimalInit(&decimal, bitwidth, 8, 3);
      ArrowDecimalSetInt(&decimal, 1);
      return ArrowArrayAppendDecimal(array, &decimal);
    }
    default:
      return EINVAL;
  }
}

/// Build and finish a one-row array whose schema retains the requested logical type.
[[nodiscard]] PJ::sdk::ArrowArrayHolder makeCopyableScalarArray(const ArrowSchema* schema, ArrowType type) {
  PJ::sdk::ArrowArrayHolder array;
  ArrowError error{};
  int result = ArrowArrayInitFromSchema(array.out(), schema, &error);
  if (result == NANOARROW_OK) {
    result = ArrowArrayStartAppending(array.get());
  }
  if (result == NANOARROW_OK) {
    result = appendCopyableScalar(array.get(), type);
  }
  if (result == NANOARROW_OK) {
    result = ArrowArrayFinishBuildingDefault(array.get(), &error);
  }
  if (result != NANOARROW_OK) {
    throw std::runtime_error(
        std::string("scalar array initialization failed for ") + ArrowTypeString(type) + ": " + error.message);
  }
  return array;
}

/// Mirror the host's schema mapping independently so the compatibility table cannot validate itself. Keyed on the
/// source type but answering for the format the host receives: large and view strings are normalized to utf8 by
/// hostFormat before their row is reached, so they answer false here exactly as STRING_VIEW does.
[[nodiscard]] constexpr bool hostRecognizes(ArrowType type) noexcept {
  switch (type) {
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
    case NANOARROW_TYPE_BOOL:
    case NANOARROW_TYPE_STRING:
      return true;
    default:
      return false;
  }
}

/// Build `<parent>: struct<<leaf>: timestamp[us]>` beside a double column.
[[nodiscard]] PJ::sdk::ArrowSchemaHolder makeNestedTimestampSchema(const char* parent_name, const char* leaf_name) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  if (ArrowSchemaSetTypeStruct(schema.get(), 2) != NANOARROW_OK ||
      ArrowSchemaSetTypeStruct(schema.get()->children[0], 1) != NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[0], parent_name) != NANOARROW_OK ||
      ArrowSchemaSetTypeDateTime(
          schema.get()->children[0]->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, nullptr) !=
          NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[0]->children[0], leaf_name) != NANOARROW_OK ||
      ArrowSchemaSetType(schema.get()->children[1], NANOARROW_TYPE_DOUBLE) != NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[1], "value") != NANOARROW_OK) {
    throw std::runtime_error("nested timestamp schema initialization failed");
  }
  return schema;
}

/// Build one two-row batch of microsecond ticks for a makeNestedTimestampSchema stream, optionally followed by a
/// third row whose whole `parent` struct is null.
[[nodiscard]] PJ::sdk::ArrowStreamHolder makeNestedTimestampStream(
    const char* parent_name, const char* leaf_name, bool null_parent_row = false) {
  auto schema = makeNestedTimestampSchema(parent_name, leaf_name);
  PJ::sdk::ArrowArrayHolder batch;
  ArrowError error{};
  if (ArrowArrayInitFromSchema(batch.out(), schema.get(), &error) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[0]) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[0]->children[0]) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[1]) != NANOARROW_OK) {
    throw std::runtime_error("nested timestamp array initialization failed");
  }
  for (int64_t row = 0; row < 2; ++row) {
    if (ArrowArrayAppendInt(batch.get()->children[0]->children[0], row + 1) != NANOARROW_OK ||
        ArrowArrayFinishElement(batch.get()->children[0]) != NANOARROW_OK ||
        ArrowArrayAppendDouble(batch.get()->children[1], static_cast<double>(row)) != NANOARROW_OK) {
      throw std::runtime_error("nested timestamp row append failed");
    }
  }
  if (null_parent_row && (ArrowArrayAppendNull(batch.get()->children[0], 1) != NANOARROW_OK ||
                          ArrowArrayAppendDouble(batch.get()->children[1], 2.0) != NANOARROW_OK)) {
    throw std::runtime_error("nested timestamp null-parent row append failed");
  }
  batch.get()->length = null_parent_row ? 3 : 2;
  batch.get()->null_count = 0;
  if (ArrowArrayFinishBuildingDefault(batch.get(), &error) != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }
  return oneBatchStream(schema.get(), batch.get());
}

/// Build a one-batch stream whose `time` axis carries the requested nanosecond ticks in `axis_type`.
[[nodiscard]] PJ::sdk::ArrowStreamHolder makeIntegerAxisStream(ArrowType axis_type, const std::vector<int64_t>& ticks) {
  auto schema = makeSchema({{"time", axis_type}, {"value", NANOARROW_TYPE_DOUBLE}});
  PJ::sdk::ArrowArrayHolder batch;
  ArrowError error{};
  if (ArrowArrayInitFromSchema(batch.out(), schema.get(), &error) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[0]) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[1]) != NANOARROW_OK) {
    throw std::runtime_error("integer axis array initialization failed");
  }
  for (const int64_t tick : ticks) {
    if (ArrowArrayAppendInt(batch.get()->children[0], tick) != NANOARROW_OK ||
        ArrowArrayAppendDouble(batch.get()->children[1], static_cast<double>(tick)) != NANOARROW_OK) {
      throw std::runtime_error("integer axis row append failed");
    }
  }
  batch.get()->length = static_cast<int64_t>(ticks.size());
  batch.get()->null_count = 0;
  if (ArrowArrayFinishBuildingDefault(batch.get(), &error) != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }
  return oneBatchStream(schema.get(), batch.get());
}

/// Build a one-batch stream whose explicit `time` axis stores floating-point seconds.
[[nodiscard]] PJ::sdk::ArrowStreamHolder makeFloatingAxisStream(
    ArrowType axis_type, const std::vector<double>& seconds) {
  auto schema = makeSchema({{"time", axis_type}, {"value", NANOARROW_TYPE_DOUBLE}});
  PJ::sdk::ArrowArrayHolder batch;
  ArrowError error{};
  if (ArrowArrayInitFromSchema(batch.out(), schema.get(), &error) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[0]) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[1]) != NANOARROW_OK) {
    throw std::runtime_error("floating axis array initialization failed");
  }
  for (const double value : seconds) {
    if (ArrowArrayAppendDouble(batch.get()->children[0], value) != NANOARROW_OK ||
        ArrowArrayAppendDouble(batch.get()->children[1], value) != NANOARROW_OK) {
      throw std::runtime_error("floating axis row append failed");
    }
  }
  batch.get()->length = static_cast<int64_t>(seconds.size());
  batch.get()->null_count = 0;
  if (ArrowArrayFinishBuildingDefault(batch.get(), &error) != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }
  return oneBatchStream(schema.get(), batch.get());
}

/// Build one row for the two-batch stream whose delayed timestamp conversion eventually fails.
[[nodiscard]] PJ::sdk::ArrowArrayHolder makeDelayedTimestampBatch(
    const ArrowSchema* schema, int64_t timestamp_ns, int64_t delayed_time) {
  PJ::sdk::ArrowArrayHolder batch;
  ArrowError error{};
  if (ArrowArrayInitFromSchema(batch.out(), schema, &error) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[0]) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[1]) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[2]) != NANOARROW_OK ||
      ArrowArrayAppendInt(batch.get()->children[0], timestamp_ns) != NANOARROW_OK ||
      ArrowArrayAppendInt(batch.get()->children[1], delayed_time) != NANOARROW_OK ||
      ArrowArrayAppendDouble(batch.get()->children[2], 1.0) != NANOARROW_OK) {
    throw std::runtime_error("delayed timestamp batch initialization failed");
  }
  batch.get()->length = 1;
  batch.get()->null_count = 0;
  if (ArrowArrayFinishBuildingDefault(batch.get(), &error) != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }
  return batch;
}

/// Build two batches where only the second overflows while converting a named data timestamp to nanoseconds.
[[nodiscard]] PJ::sdk::ArrowStreamHolder makeSecondBatchTimestampOverflowStream() {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  if (ArrowSchemaSetTypeStruct(schema.get(), 3) != NANOARROW_OK ||
      ArrowSchemaSetType(schema.get()->children[0], NANOARROW_TYPE_INT64) != NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[0], "timestamp_ns") != NANOARROW_OK ||
      ArrowSchemaSetTypeDateTime(
          schema.get()->children[1], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_SECOND, nullptr) != NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[1], "delayed_time") != NANOARROW_OK ||
      ArrowSchemaSetType(schema.get()->children[2], NANOARROW_TYPE_DOUBLE) != NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[2], "value") != NANOARROW_OK) {
    throw std::runtime_error("delayed timestamp schema initialization failed");
  }

  auto first_batch = makeDelayedTimestampBatch(schema.get(), 1, 1);
  auto second_batch = makeDelayedTimestampBatch(schema.get(), 2, std::numeric_limits<int64_t>::max());
  ArrowArrayStream stream{};
  if (ArrowBasicArrayStreamInit(&stream, schema.get(), 2) != NANOARROW_OK) {
    throw std::runtime_error("ArrowBasicArrayStreamInit failed");
  }
  ArrowBasicArrayStreamSetArray(&stream, 0, first_batch.get());
  ArrowBasicArrayStreamSetArray(&stream, 1, second_batch.get());
  return PJ::sdk::ArrowStreamHolder(stream);
}

/// Build two rows whose timestamp-list columns fail in opposite row/column order.
[[nodiscard]] PJ::sdk::ArrowStreamHolder makeCompetingTimestampListOverflowStream() {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  if (ArrowSchemaSetTypeStruct(schema.get(), 3) != NANOARROW_OK ||
      ArrowSchemaSetType(schema.get()->children[0], NANOARROW_TYPE_INT64) != NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[0], "timestamp_ns") != NANOARROW_OK ||
      ArrowSchemaSetTypeFixedSize(schema.get()->children[1], NANOARROW_TYPE_FIXED_SIZE_LIST, 2) != NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[1], "times") != NANOARROW_OK ||
      ArrowSchemaSetTypeDateTime(
          schema.get()->children[1]->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_SECOND, nullptr) !=
          NANOARROW_OK ||
      ArrowSchemaSetType(schema.get()->children[2], NANOARROW_TYPE_DOUBLE) != NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[2], "value") != NANOARROW_OK) {
    throw std::runtime_error("timestamp-list schema initialization failed");
  }

  PJ::sdk::ArrowArrayHolder batch;
  ArrowError error{};
  if (ArrowArrayInitFromSchema(batch.out(), schema.get(), &error) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[0]) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[1]) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[1]->children[0]) != NANOARROW_OK ||
      ArrowArrayStartAppending(batch.get()->children[2]) != NANOARROW_OK) {
    throw std::runtime_error("timestamp-list array initialization failed");
  }

  constexpr int64_t kOverflowingSeconds = std::numeric_limits<int64_t>::max();
  for (int64_t row = 0; row < 2; ++row) {
    int result = ArrowArrayAppendInt(batch.get()->children[0], 1000 + row);
    const int64_t first = row == 0 ? 1 : kOverflowingSeconds;
    const int64_t second = row == 0 ? kOverflowingSeconds : 1;
    if (result == NANOARROW_OK) {
      result = ArrowArrayAppendInt(batch.get()->children[1]->children[0], first);
    }
    if (result == NANOARROW_OK) {
      result = ArrowArrayAppendInt(batch.get()->children[1]->children[0], second);
    }
    if (result == NANOARROW_OK) {
      result = ArrowArrayFinishElement(batch.get()->children[1]);
    }
    if (result == NANOARROW_OK) {
      result = ArrowArrayAppendDouble(batch.get()->children[2], static_cast<double>(row));
    }
    if (result != NANOARROW_OK) {
      throw std::runtime_error("timestamp-list row append failed");
    }
  }
  batch.get()->length = 2;
  batch.get()->null_count = 0;
  if (ArrowArrayFinishBuildingDefault(batch.get(), &error) != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }
  return oneBatchStream(schema.get(), batch.get());
}

/// Build a nullable axis with an earlier overflow either in the axis itself or in a preceding data timestamp.
[[nodiscard]] PJ::sdk::ArrowStreamHolder makeAxisNullPrecedenceStream(bool earlier_non_axis_timestamp) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  const int64_t field_count = earlier_non_axis_timestamp ? 3 : 2;
  if (ArrowSchemaSetTypeStruct(schema.get(), field_count) != NANOARROW_OK) {
    throw std::runtime_error("axis-null schema initialization failed");
  }
  int64_t axis_index = 0;
  if (earlier_non_axis_timestamp) {
    if (ArrowSchemaSetTypeDateTime(
            schema.get()->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_SECOND, nullptr) != NANOARROW_OK ||
        ArrowSchemaSetName(schema.get()->children[0], "data_time") != NANOARROW_OK) {
      throw std::runtime_error("data timestamp schema initialization failed");
    }
    axis_index = 1;
  } else if (
      ArrowSchemaSetTypeDateTime(
          schema.get()->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_SECOND, nullptr) != NANOARROW_OK) {
    throw std::runtime_error("timestamp axis schema initialization failed");
  }
  if (earlier_non_axis_timestamp) {
    if (ArrowSchemaSetType(schema.get()->children[axis_index], NANOARROW_TYPE_INT64) != NANOARROW_OK) {
      throw std::runtime_error("timestamp axis schema initialization failed");
    }
  }
  if (ArrowSchemaSetName(schema.get()->children[axis_index], "time") != NANOARROW_OK ||
      ArrowSchemaSetType(schema.get()->children[field_count - 1], NANOARROW_TYPE_DOUBLE) != NANOARROW_OK ||
      ArrowSchemaSetName(schema.get()->children[field_count - 1], "value") != NANOARROW_OK) {
    throw std::runtime_error("axis-null schema naming failed");
  }
  schema.get()->children[axis_index]->flags |= ARROW_FLAG_NULLABLE;

  PJ::sdk::ArrowArrayHolder batch;
  ArrowError error{};
  if (ArrowArrayInitFromSchema(batch.out(), schema.get(), &error) != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }
  for (int64_t child = 0; child < field_count; ++child) {
    if (ArrowArrayStartAppending(batch.get()->children[child]) != NANOARROW_OK) {
      throw std::runtime_error("axis-null array initialization failed");
    }
  }

  constexpr int64_t kOverflowingSeconds = std::numeric_limits<int64_t>::max();
  for (int64_t row = 0; row < 2; ++row) {
    int result = NANOARROW_OK;
    if (earlier_non_axis_timestamp) {
      result = ArrowArrayAppendInt(batch.get()->children[0], row == 0 ? kOverflowingSeconds : 1);
    }
    if (result == NANOARROW_OK && row == 0) {
      result =
          ArrowArrayAppendInt(batch.get()->children[axis_index], earlier_non_axis_timestamp ? 1 : kOverflowingSeconds);
    } else if (result == NANOARROW_OK) {
      result = ArrowArrayAppendNull(batch.get()->children[axis_index], 1);
    }
    if (result == NANOARROW_OK) {
      result = ArrowArrayAppendDouble(batch.get()->children[field_count - 1], static_cast<double>(row));
    }
    if (result != NANOARROW_OK) {
      throw std::runtime_error("axis-null row append failed");
    }
  }
  batch.get()->length = 2;
  batch.get()->null_count = 0;
  if (ArrowArrayFinishBuildingDefault(batch.get(), &error) != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }
  return oneBatchStream(schema.get(), batch.get());
}

/// State for a stream whose schema succeeds and first record-batch pull fails.
struct FailingPeekState {
  PJ::sdk::ArrowSchemaHolder schema;
};

int failingPeekGetSchema(ArrowArrayStream* stream, ArrowSchema* output) noexcept {
  const auto* state = static_cast<const FailingPeekState*>(stream->private_data);
  return ArrowSchemaDeepCopy(state->schema.get(), output);
}

int failingPeekGetNext(ArrowArrayStream*, ArrowArray* output) noexcept {
  *output = {};
  return EIO;
}

const char* failingPeekGetLastError(ArrowArrayStream*) noexcept {
  return "configured first-batch failure";
}

void failingPeekRelease(ArrowArrayStream* stream) noexcept {
  delete static_cast<FailingPeekState*>(stream->private_data);
  *stream = {};
}

/// Build a stream used to verify that variable-list peeking errors are synchronous.
[[nodiscard]] PJ::sdk::ArrowStreamHolder makeFailingPeekStream(PJ::sdk::ArrowSchemaHolder schema) {
  auto* state = new FailingPeekState{std::move(schema)};
  ArrowArrayStream stream{};
  stream.get_schema = &failingPeekGetSchema;
  stream.get_next = &failingPeekGetNext;
  stream.get_last_error = &failingPeekGetLastError;
  stream.release = &failingPeekRelease;
  stream.private_data = state;
  return PJ::sdk::ArrowStreamHolder(stream);
}

/// Return a schema child index by exact output name or fail the current test.
[[nodiscard]] int64_t childIndex(const ArrowSchema* schema, std::string_view name) {
  for (int64_t index = 0; index < schema->n_children; ++index) {
    if (schema->children[index] != nullptr && schema->children[index]->name != nullptr &&
        std::string_view(schema->children[index]->name) == name) {
      return index;
    }
  }
  ADD_FAILURE() << "Missing schema child: " << name;
  return 0;
}

/// Return one plan-time warning by stable code so tests remain insensitive to unrelated warning order.
[[nodiscard]] const ShapeWarning* warningWithCode(const ShapedStream& shaped, std::string_view code) noexcept {
  for (const auto& warning : shaped.warnings) {
    if (warning.code == code) {
      return &warning;
    }
  }
  return nullptr;
}

/// Re-encode decoded canonical storage as C-Data views because nanoarrow 0.7 cannot decode view IPC batches.
[[nodiscard]] PJ::sdk::ArrowStreamHolder makeViewStream(PJ::sdk::ArrowStreamHolder storage_stream) {
  auto storage_schema = readSchema(storage_stream);
  auto storage_batch = readBatch(storage_stream);
  auto storage_view = test::bindArrayView(storage_schema.get(), storage_batch.get());

  auto schema = makeSchema(
      {{"timestamp_ns", NANOARROW_TYPE_INT64},
       {"label", NANOARROW_TYPE_STRING_VIEW},
       {"blob", NANOARROW_TYPE_BINARY_VIEW},
       {"value", NANOARROW_TYPE_DOUBLE}});
  PJ::sdk::ArrowArrayHolder batch;
  ArrowError error{};
  int result = ArrowArrayInitFromSchema(batch.out(), schema.get(), &error);
  if (result != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }
  for (int64_t child_index = 0; child_index < batch.get()->n_children; ++child_index) {
    result = ArrowArrayStartAppending(batch.get()->children[child_index]);
    if (result != NANOARROW_OK) {
      throw std::runtime_error("ArrowArrayStartAppending failed");
    }
  }

  for (int64_t row = 0; row < storage_batch.get()->length; ++row) {
    result =
        ArrowArrayAppendInt(batch.get()->children[0], ArrowArrayViewGetIntUnsafe(storage_view.get()->children[0], row));
    if (result == NANOARROW_OK && ArrowArrayViewIsNull(storage_view.get()->children[1], row) != 0) {
      result = ArrowArrayAppendNull(batch.get()->children[1], 1);
    } else if (result == NANOARROW_OK) {
      result = ArrowArrayAppendBytes(
          batch.get()->children[1], ArrowArrayViewGetBytesUnsafe(storage_view.get()->children[1], row));
    }
    if (result == NANOARROW_OK && ArrowArrayViewIsNull(storage_view.get()->children[2], row) != 0) {
      result = ArrowArrayAppendNull(batch.get()->children[2], 1);
    } else if (result == NANOARROW_OK) {
      result = ArrowArrayAppendBytes(
          batch.get()->children[2], ArrowArrayViewGetBytesUnsafe(storage_view.get()->children[2], row));
    }
    if (result == NANOARROW_OK) {
      result = ArrowArrayAppendDouble(
          batch.get()->children[3], ArrowArrayViewGetDoubleUnsafe(storage_view.get()->children[3], row));
    }
    if (result != NANOARROW_OK) {
      throw std::runtime_error("failed to append view fixture row");
    }
  }
  batch.get()->length = storage_batch.get()->length;
  batch.get()->null_count = 0;
  result = ArrowArrayFinishBuildingDefault(batch.get(), &error);
  if (result != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }

  ArrowArrayStream raw_stream{};
  result = ArrowBasicArrayStreamInit(&raw_stream, schema.get(), 1);
  if (result != NANOARROW_OK) {
    throw std::runtime_error("ArrowBasicArrayStreamInit failed");
  }
  ArrowBasicArrayStreamSetArray(&raw_stream, 0, batch.get());
  return PJ::sdk::ArrowStreamHolder(raw_stream);
}

/// Every accepted logical type must resolve to storage handled by the production scalar append dispatch.
TEST(TableShaperTest, EveryCopyableTypeHasAnAppendPath) {
  std::size_t exercised = 0;
  for (const TypeRow& row : kTypeTable) {
    SCOPED_TRACE(ArrowTypeString(row.type));
    EXPECT_EQ(test::supportsScalarCopyForTesting(row.type), row.copy != CopyKind::kUnsupported);
    if (row.copy == CopyKind::kUnsupported) {
      continue;
    }
    auto schema = makeCopyableScalarSchema(row.type);
    auto input = makeCopyableScalarArray(schema.get(), row.type);
    auto input_view = test::bindArrayView(schema.get(), input.get());

    PJ::sdk::ArrowArrayHolder output;
    ArrowError error{};
    ASSERT_EQ(ArrowArrayInitFromSchema(output.out(), schema.get(), &error), NANOARROW_OK) << error.message;
    ASSERT_EQ(ArrowArrayStartAppending(output.get()), NANOARROW_OK);
    ASSERT_EQ(test::appendCastedValueForTesting(output.get(), input_view.get(), 0, schema.get()), NANOARROW_OK);
    EXPECT_EQ(ArrowArrayFinishBuildingDefault(output.get(), &error), NANOARROW_OK) << error.message;
    ++exercised;
  }
  EXPECT_EQ(exercised, 33U);
}

/// Keep the compatibility table synchronized with the format the host actually receives.
TEST(TableShaperTest, HostIngestibleMatchesHostMapping) {
  for (const TypeRow& row : kTypeTable) {
    SCOPED_TRACE(ArrowTypeString(row.type));
    EXPECT_EQ(row.host_ingestible, hostRecognizes(row.type));
  }

  // LARGE_STRING's host_ingestible is never consulted, so pin the live path instead: it is normalized to utf8 and
  // reaches the host, which the flag alone would deny.
  auto shaped = shapeStream(decodeFixture("large_types.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 3);
  EXPECT_STREQ(schema.get()->children[1]->name, "label");
  EXPECT_STREQ(schema.get()->children[1]->format, "u");
}

/// Automatic selection must never admit a type that explicit axis configuration cannot convert.
TEST(TableShaperTest, AutoAxisPlausibleIsSubsetOfAxisCast) {
  for (const TypeRow& row : kTypeTable) {
    SCOPED_TRACE(ArrowTypeString(row.type));
    if (row.auto_axis_plausible) {
      EXPECT_TRUE(row.axis_cast.has_value());
    }
  }
}

/// A duplicate row would make typeRow's first-match behavior silently order-dependent.
TEST(TableShaperTest, RowsAreUnique) {
  for (std::size_t left = 0; left < kTypeTable.size(); ++left) {
    for (std::size_t right = left + 1; right < kTypeTable.size(); ++right) {
      SCOPED_TRACE(ArrowTypeString(kTypeTable[left].type));
      EXPECT_NE(kTypeTable[left].type, kTypeTable[right].type);
    }
  }
}

/// Name heuristics use their specified global priority, not schema order.
TEST(TableShaperTest, DetectsTimestampNamesInPriorityOrder) {
  auto with_timestamp =
      makeSchema({{"ts", NANOARROW_TYPE_INT64}, {"time", NANOARROW_TYPE_INT64}, {"timestamp", NANOARROW_TYPE_INT64}});
  auto shaped = shapeStream(emptyStream(with_timestamp.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "timestamp");

  auto with_time = makeSchema({{"ts", NANOARROW_TYPE_INT64}, {"time", NANOARROW_TYPE_INT64}});
  shaped = shapeStream(emptyStream(with_time.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "time");
}

/// Name priority applies only after implausibly narrow candidates have been discarded.
TEST(TableShaperTest, PrefersPlausibleAxisOverHigherPriorityNarrowName) {
  auto schema = makeSchema({{"timestamp_ns", NANOARROW_TYPE_INT8}, {"time", NANOARROW_TYPE_INT64}});
  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "time");
  EXPECT_TRUE(shaped->warnings.empty());
}

/// The named pass accepts exactly the scalar types whose epoch range and precision are plausible.
TEST(TableShaperTest, AutoDetectsOnlyPlausibleNamedScalarTypes) {
  struct AxisCase {
    ArrowType type;
    bool plausible;
  };
  constexpr std::array<AxisCase, 10> kCases = {{
      {NANOARROW_TYPE_INT8, false},
      {NANOARROW_TYPE_INT16, false},
      {NANOARROW_TYPE_INT32, false},
      {NANOARROW_TYPE_INT64, true},
      {NANOARROW_TYPE_UINT8, false},
      {NANOARROW_TYPE_UINT16, false},
      {NANOARROW_TYPE_UINT32, false},
      {NANOARROW_TYPE_UINT64, true},
      {NANOARROW_TYPE_FLOAT, false},
      {NANOARROW_TYPE_DOUBLE, true},
  }};
  for (const auto& axis_case : kCases) {
    SCOPED_TRACE(ArrowTypeString(axis_case.type));
    auto schema = makeSchema({{"timestamp_ns", axis_case.type}, {"time", NANOARROW_TYPE_INT64}});
    auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
    ASSERT_TRUE(shaped) << shaped.error();
    EXPECT_EQ(shaped->timestamp_column, axis_case.plausible ? "timestamp_ns" : "time");
    EXPECT_TRUE(shaped->warnings.empty());
  }
}

/// A list's element type cannot make the list itself a plausible named axis.
TEST(TableShaperTest, NeverSelectsNamedListElementAsTimestampAxis) {
  auto schema = makeSchema(
      {{"timestamp", NANOARROW_TYPE_LIST}, {"time", NANOARROW_TYPE_INT64}, {"value", NANOARROW_TYPE_DOUBLE}});
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[0]->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);

  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "time");
}

/// An implausible same-name list cannot mask a later plausible scalar because filtering is candidate-based.
TEST(TableShaperTest, FindsPlausibleScalarAfterSameNameList) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 3), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetTypeFixedSize(schema.get()->children[0], NANOARROW_TYPE_FIXED_SIZE_LIST, 1), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[0], "time"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[0]->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[1], "time"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[2], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[2], "value"), NANOARROW_OK);

  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "time");
  auto output_schema = readSchema(shaped->stream);
  EXPECT_STREQ(output_schema.get()->children[0]->name, "time[0]");
  EXPECT_STREQ(output_schema.get()->children[1]->name, "time");
}

/// Heuristic names match the whole flattened path, so a nested `timestamp_ns` loses to a top-level `ts`.
TEST(TableShaperTest, MatchesHeuristicNamesAgainstWholeLeafPath) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 2), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get()->children[0], 1), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[0], "x"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[0]->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[0]->children[0], "timestamp_ns"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[1], "ts"), NANOARROW_OK);

  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "ts");
}

/// Schemas without typed or named candidates fall through to a synthesized axis.
TEST(TableShaperTest, SynthesizesAxisWithoutDetectionCandidate) {
  auto schema = makeSchema({{"a", NANOARROW_TYPE_DOUBLE}, {"b", NANOARROW_TYPE_INT32}});
  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "timestamp_ns");
  EXPECT_TRUE(shaped->synthetic_axis);
  const auto* warning = warningWithCode(*shaped, "parser_arrow.synthetic_timestamp");
  ASSERT_NE(warning, nullptr);
  EXPECT_EQ(
      warning->message, "no plausible timestamp axis found; using synthetic timestamp_ns with synthetic_interval_ns=0");
  auto output_schema = readSchema(shaped->stream);
  ASSERT_EQ(output_schema.get()->n_children, 3);
  EXPECT_STREQ(output_schema.get()->children[0]->name, "timestamp_ns");
}

/// An explicit existing timestamp field overrides automatic type detection.
TEST(TableShaperTest, UsesExplicitTimestampColumnAsIs) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto shaped = shapeStream(decodeFixture("timestamp_typed.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "time");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[1]->name, "time");
  EXPECT_STREQ(schema.get()->children[1]->format, "l");
}

/// A dotted configured name normalizes like the leaf it targets, so the producer's own spelling still selects it.
TEST(TableShaperTest, AcceptsDottedConfiguredTimestampColumn) {
  ShapeOptions options;
  options.timestamp_column = "sensor.time";
  auto schema = makeSchema({{"sensor.time", NANOARROW_TYPE_INT64}, {"value", NANOARROW_TYPE_DOUBLE}});
  auto shaped = shapeStream(emptyStream(schema.get()), options);
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "sensor/time");
  auto output_schema = readSchema(shaped->stream);
  EXPECT_STREQ(output_schema.get()->children[0]->name, "sensor/time");
}

/// A missing explicit timestamp is an error instead of an automatic fallback.
TEST(TableShaperTest, RejectsMissingExplicitTimestampColumn) {
  ShapeOptions options;
  options.timestamp_column = "does_not_exist";
  auto shaped = shapeStream(decodeFixture("flat.arrows"), options);
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("parser_arrow:"), std::string::npos);
  EXPECT_NE(shaped.error().find("does_not_exist"), std::string::npos);
}

/// An empty option auto-detects, and a TIMESTAMP-typed field outranks the fixture's `time` name decoy.
TEST(TableShaperTest, AutoDetectsTimestampColumn) {
  auto shaped = shapeStream(decodeFixture("timestamp_typed.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "stamp");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->name, "stamp");
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
}

/// Detection walks flattened leaves, so a nested timestamp becomes the axis and is scaled to int64 nanoseconds.
TEST(TableShaperTest, UsesNestedTimestampLeafAsAxis) {
  auto shaped = shapeStream(makeNestedTimestampStream("header", "stamp"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "header/stamp");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 2);
  EXPECT_STREQ(schema.get()->children[0]->name, "header/stamp");
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 1000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), 2000);
}

/// A null struct ancestor nulls the nested axis, which is rejected instead of reaching the host as a raw value.
TEST(TableShaperTest, RejectsNestedTimestampUnderNullStructParent) {
  auto shaped = shapeStream(makeNestedTimestampStream("header", "stamp", true), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), EINVAL);
  EXPECT_STREQ(
      ArrowArrayStreamGetLastError(shaped->stream.get()),
      "parser_arrow: timestamp column 'header/stamp' contains a null value");
}

/// The configured timestamp column names a leaf by its flattened path at any depth.
TEST(TableShaperTest, UsesConfiguredNestedTimestampLeaf) {
  ShapeOptions options;
  options.timestamp_column = "header/stamp";
  auto shaped = shapeStream(makeNestedTimestampStream("header", "stamp"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "header/stamp");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->name, "header/stamp");
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
}

/// Dots inside a field name are path separators, matching PlotJuggler 3's series names.
TEST(TableShaperTest, NormalizesDottedFieldNameToSlashes) {
  auto schema = makeSchema({{"timestamp_ns", NANOARROW_TYPE_INT64}, {"wheel.speed", NANOARROW_TYPE_DOUBLE}});
  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto output_schema = readSchema(shaped->stream);
  ASSERT_EQ(output_schema.get()->n_children, 2);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "wheel/speed");
}

/// A dotted flat name colliding with the equivalent struct is rejected: the whole stream fails, it is not deduplicated.
TEST(TableShaperTest, RejectsDottedNameCollidingWithStructLeaf) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 3), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[0], "timestamp_ns"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[1], "a.b"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get()->children[2], 1), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[2], "a"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[2]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[2]->children[0], "b"), NANOARROW_OK);

  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("duplicate output column name 'a/b'"), std::string::npos);
}

/// Dotted components normalize at every flatten depth and stay detectable as the axis.
TEST(TableShaperTest, NormalizesDottedFieldNameInsideStruct) {
  auto shaped = shapeStream(makeNestedTimestampStream("msg", "header.stamp"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "msg/header/stamp");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->name, "msg/header/stamp");
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
}

/// Missing timestamps produce a prepended int64 timestamp_ns sequence.
TEST(TableShaperTest, SynthesizesTimestampColumnForEveryRow) {
  ShapeOptions options;
  options.message_timestamp_ns = 5000;
  options.synthetic_interval_ns = 7;
  auto shaped = shapeStream(decodeFixture("no_timestamp.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "timestamp_ns");
  EXPECT_TRUE(shaped->synthetic_axis);
  const auto* warning = warningWithCode(*shaped, "parser_arrow.synthetic_timestamp");
  ASSERT_NE(warning, nullptr);
  EXPECT_NE(warning->message.find("synthetic_interval_ns=7"), std::string::npos);

  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 3);
  EXPECT_STREQ(schema.get()->children[0]->name, "timestamp_ns");
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 5000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), 5007);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 5014);
}

/// The synthetic row index continues across record-batch boundaries.
TEST(TableShaperTest, ContinuesSyntheticTimestampAcrossBatches) {
  ShapeOptions options;
  options.message_timestamp_ns = 5000;
  options.synthetic_interval_ns = 7;
  auto shaped = shapeStream(decodeFixture("no_timestamp_two_batches.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);

  auto first_batch = readBatch(shaped->stream);
  auto first_view = test::bindArrayView(schema.get(), first_batch.get());
  ASSERT_EQ(first_batch.get()->length, 2);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(first_view.get()->children[0], 0), 5000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(first_view.get()->children[0], 1), 5007);

  auto second_batch = readBatch(shaped->stream);
  auto second_view = test::bindArrayView(schema.get(), second_batch.get());
  ASSERT_EQ(second_batch.get()->length, 1);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(second_view.get()->children[0], 0), 5014);
}

/// A zero synthetic interval repeats the message timestamp for every row.
TEST(TableShaperTest, RepeatsAnchorWhenSyntheticIntervalIsZero) {
  ShapeOptions options;
  options.message_timestamp_ns = 5000;
  auto shaped = shapeStream(decodeFixture("no_timestamp.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  for (int64_t row = 0; row < batch.get()->length; ++row) {
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], row), 5000);
  }
}

/// Positive synthetic sequences report the exact overflowing stream row.
TEST(TableShaperTest, ReportsPositiveSyntheticTimestampOverflow) {
  ShapeOptions options;
  options.message_timestamp_ns = std::numeric_limits<int64_t>::max();
  options.synthetic_interval_ns = 1;
  auto shaped = shapeStream(decodeFixture("no_timestamp.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();

  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), ERANGE);
  EXPECT_NE(
      std::string(ArrowArrayStreamGetLastError(shaped->stream.get())).find("synthetic timestamp overflow at row 1"),
      std::string::npos);
}

/// Negative synthetic sequences report underflow with the same row-specific diagnostic.
TEST(TableShaperTest, ReportsNegativeSyntheticTimestampOverflow) {
  ShapeOptions options;
  options.message_timestamp_ns = std::numeric_limits<int64_t>::min();
  options.synthetic_interval_ns = -1;
  auto shaped = shapeStream(decodeFixture("no_timestamp.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();

  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), ERANGE);
  EXPECT_NE(
      std::string(ArrowArrayStreamGetLastError(shaped->stream.get())).find("synthetic timestamp overflow at row 1"),
      std::string::npos);
}

/// Typed nanosecond timestamp axes are exposed to the host as int64 nanoseconds.
TEST(TableShaperTest, CastsTypedTimestampAxisToInt64Nanoseconds) {
  auto shaped = shapeStream(decodeFixture("timestamp_typed.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "stamp");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 1000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), 2000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 3000);
}

/// Timestamp units are scaled and every typed timestamp data column is retained as int64.
TEST(TableShaperTest, ScalesAllTypedTimestampColumnsToNanoseconds) {
  auto shaped = shapeStream(decodeFixture("timestamp_casts.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  EXPECT_STREQ(schema.get()->children[1]->format, "l");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 1'000'000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 3'000'000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[1], 0), 4000);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[1], 1));
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[1], 2), 6000);
}

/// Nested structs flatten depth-first into slash-separated leaves with preserved values.
TEST(TableShaperTest, FlattensNestedStructsDepthFirst) {
  auto shaped = shapeStream(decodeFixture("nested.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  constexpr std::array<std::string_view, 9> kNames = {"timestamp_ns",       "pose/position/x",    "pose/position/y",
                                                      "pose/position/z",    "pose/orientation/x", "pose/orientation/y",
                                                      "pose/orientation/z", "pose/orientation/w", "speed"};
  ASSERT_EQ(schema.get()->n_children, static_cast<int64_t>(kNames.size()));
  for (std::size_t index = 0; index < kNames.size(); ++index) {
    EXPECT_EQ(schema.get()->children[index]->name, kNames[index]);
  }

  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  constexpr std::array<std::array<double, 7>, 2> kPoseValues = {
      std::array<double, 7>{1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 0.4},
      std::array<double, 7>{4.0, 5.0, 6.0, 0.5, 0.6, 0.7, 0.8}};
  for (std::size_t row = 0; row < kPoseValues.size(); ++row) {
    for (std::size_t leaf = 0; leaf < kPoseValues[row].size(); ++leaf) {
      EXPECT_DOUBLE_EQ(
          ArrowArrayViewGetDoubleUnsafe(view.get()->children[leaf + 1], static_cast<int64_t>(row)),
          kPoseValues[row][leaf]);
    }
  }
}

/// Flattened leaves become nullable when any source struct ancestor is nullable.
TEST(TableShaperTest, MarksLeafNullableWhenStructAncestorIsNullable) {
  PJ::sdk::ArrowSchemaHolder input_schema;
  ArrowSchemaInit(input_schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(input_schema.get(), 2), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(input_schema.get()->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(input_schema.get()->children[0], "timestamp_ns"), NANOARROW_OK);

  auto* parent = input_schema.get()->children[1];
  ASSERT_EQ(ArrowSchemaSetTypeStruct(parent, 1), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(parent, "pose"), NANOARROW_OK);
  parent->flags |= ARROW_FLAG_NULLABLE;
  ASSERT_EQ(ArrowSchemaSetType(parent->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(parent->children[0], "x"), NANOARROW_OK);
  parent->children[0]->flags &= ~ARROW_FLAG_NULLABLE;

  ArrowArrayStream raw_stream{};
  ASSERT_EQ(ArrowBasicArrayStreamInit(&raw_stream, input_schema.get(), 0), NANOARROW_OK);
  auto shaped = shapeStream(PJ::sdk::ArrowStreamHolder(raw_stream), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto output_schema = readSchema(shaped->stream);
  ASSERT_EQ(output_schema.get()->n_children, 2);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "pose/x");
  EXPECT_NE(output_schema.get()->children[1]->flags & ARROW_FLAG_NULLABLE, 0);
}

/// Null parent struct slots become null in every flattened descendant leaf.
TEST(TableShaperTest, PropagatesNullStructParentToFlattenedLeaves) {
  auto shaped = shapeStream(decodeFixture("nested.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  for (int64_t child = 1; child <= 7; ++child) {
    EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[child], 2));
  }
  EXPECT_FALSE(ArrowArrayViewIsNull(view.get()->children[8], 2));
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[8], 2), 7.5);
}

/// A sliced record-batch struct applies its parent offset before copying flattened leaves.
TEST(TableShaperTest, AppliesSlicedStructParentOffsetWhenFlattening) {
  auto decoded = decodeFixture("nested.arrows");
  auto input_schema = readSchema(decoded);
  auto input_batch = readBatch(decoded);
  input_batch.get()->offset = 1;
  input_batch.get()->length = 2;

  ArrowArrayStream raw_stream{};
  ASSERT_EQ(ArrowBasicArrayStreamInit(&raw_stream, input_schema.get(), 1), NANOARROW_OK);
  ArrowBasicArrayStreamSetArray(&raw_stream, 0, input_batch.get());
  auto shaped = shapeStream(PJ::sdk::ArrowStreamHolder(raw_stream), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();

  auto schema = readSchema(shaped->stream);
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 2000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), 3000);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 0), 4.0);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[1], 1));
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[8], 0), 6.5);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[8], 1), 7.5);
}

/// Disabling flattening removes the unsupported struct while preserving supported siblings.
TEST(TableShaperTest, RemovesStructColumnWhenFlatteningDisabled) {
  ShapeOptions options;
  options.flatten_structs = false;
  auto shaped = shapeStream(decodeFixture("nested.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 1);
  EXPECT_EQ(shaped->dropped_columns[0].name, "pose");
  EXPECT_EQ(shaped->dropped_columns[0].format, "+s");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 2);
  EXPECT_STREQ(schema.get()->children[0]->name, "timestamp_ns");
  EXPECT_STREQ(schema.get()->children[1]->name, "speed");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 0), 5.5);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 2), 7.5);
}

/// A string view is normalized while the unsupported normalized binary view is removed.
TEST(TableShaperTest, NormalizesStringAndBinaryViews) {
  auto shaped = shapeStream(makeViewStream(decodeFixture("views_storage.arrows")), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 1);
  EXPECT_EQ(shaped->dropped_columns[0].name, "blob");
  EXPECT_EQ(shaped->dropped_columns[0].format, "z");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 3);
  EXPECT_STREQ(schema.get()->children[1]->format, "u");
  EXPECT_STREQ(schema.get()->children[2]->name, "value");

  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  const ArrowStringView long_label = ArrowArrayViewGetStringUnsafe(view.get()->children[1], 1);
  EXPECT_EQ(
      std::string_view(long_label.data, static_cast<std::size_t>(long_label.size_bytes)),
      "this string is longer than twelve bytes");
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[1], 2));
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[2], 3), 4.25);
}

/// Canonical string remains observable while canonical binary is reported and absent from the shaped schema.
TEST(TableShaperTest, LeavesCanonicalVariableWidthColumnsInPlace) {
  auto schema = makeSchema(
      {{"timestamp_ns", NANOARROW_TYPE_INT64},
       {"string", NANOARROW_TYPE_STRING},
       {"binary", NANOARROW_TYPE_BINARY},
       {"value", NANOARROW_TYPE_DOUBLE}});
  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 1);
  EXPECT_EQ(shaped->dropped_columns[0].name, "binary");
  EXPECT_EQ(shaped->dropped_columns[0].format, "z");
  auto output_schema = readSchema(shaped->stream);
  ASSERT_EQ(output_schema.get()->n_children, 3);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "string");
  EXPECT_STREQ(output_schema.get()->children[1]->format, "u");
  EXPECT_STREQ(output_schema.get()->children[2]->name, "value");
}

/// Large string offsets are normalized while normalized large binary is removed.
TEST(TableShaperTest, NormalizesLargeStringAndBinaryColumns) {
  auto shaped = shapeStream(decodeFixture("large_types.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 1);
  EXPECT_EQ(shaped->dropped_columns[0].name, "blob");
  EXPECT_EQ(shaped->dropped_columns[0].format, "z");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 3);
  EXPECT_STREQ(schema.get()->children[1]->format, "u");
  EXPECT_STREQ(schema.get()->children[2]->name, "value");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  const ArrowStringView label = ArrowArrayViewGetStringUnsafe(view.get()->children[1], 1);
  EXPECT_EQ(std::string_view(label.data, static_cast<std::size_t>(label.size_bytes)), "this is a large string value");
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[1], 2));
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[2], 2), 3.0);
}

/// Primitive variable and fixed-size lists expand to scalar columns with null padding.
TEST(TableShaperTest, ExpandsPrimitiveListsWithExactValuesAndNullPadding) {
  auto shaped = shapeStream(decodeFixture("lists.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_TRUE(shaped->dropped_columns.empty());
  auto schema = readSchema(shaped->stream);
  const std::vector<std::string_view> expected_names = {"timestamp_ns", "ranges[0]", "ranges[1]", "ranges[2]",
                                                        "cov[0]",       "cov[1]",    "cov[2]",    "flags[0]",
                                                        "flags[1]",     "names[0]",  "names[1]"};
  ASSERT_EQ(schema.get()->n_children, static_cast<int64_t>(expected_names.size()));
  for (std::size_t index = 0; index < expected_names.size(); ++index) {
    EXPECT_EQ(schema.get()->children[index]->name, expected_names[index]);
  }

  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[childIndex(schema.get(), "ranges[0]")], 0), 1.0);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[childIndex(schema.get(), "ranges[2]")], 0), 3.0);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[childIndex(schema.get(), "ranges[0]")], 1), 4.0);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[childIndex(schema.get(), "ranges[2]")], 1));
  for (std::string_view name : {"ranges[0]", "ranges[1]", "ranges[2]"}) {
    EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[childIndex(schema.get(), name)], 2));
  }

  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[childIndex(schema.get(), "cov[1]")], 0), 0.2);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[childIndex(schema.get(), "cov[2]")], 1), 1.3);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[childIndex(schema.get(), "cov[0]")], 2));
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[childIndex(schema.get(), "flags[0]")], 0), 1);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[childIndex(schema.get(), "flags[1]")], 0), 0);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[childIndex(schema.get(), "flags[1]")], 1));

  const auto first_name = ArrowArrayViewGetStringUnsafe(view.get()->children[childIndex(schema.get(), "names[0]")], 0);
  EXPECT_EQ(std::string_view(first_name.data, static_cast<std::size_t>(first_name.size_bytes)), "alice");
  const auto third_name = ArrowArrayViewGetStringUnsafe(view.get()->children[childIndex(schema.get(), "names[0]")], 1);
  EXPECT_EQ(std::string_view(third_name.data, static_cast<std::size_t>(third_name.size_bytes)), "carol");
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[childIndex(schema.get(), "names[1]")], 1));
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[childIndex(schema.get(), "names[0]")], 2));
}

/// Variable-list width is fixed from the first batch and later extra elements are truncated and counted.
TEST(TableShaperTest, UsesFirstBatchWidthAndClampsLaterRows) {
  auto shaped = shapeStream(decodeFixture("lists_two_batches.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_NE(shaped->runtime, nullptr);
  EXPECT_EQ(shaped->runtime->rows_truncated, 0);
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 3);
  EXPECT_STREQ(schema.get()->children[1]->name, "ranges[0]");
  EXPECT_STREQ(schema.get()->children[2]->name, "ranges[1]");

  auto first_batch = readBatch(shaped->stream);
  auto first_view = test::bindArrayView(schema.get(), first_batch.get());
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(first_view.get()->children[1], 0), 1.0);
  EXPECT_TRUE(ArrowArrayViewIsNull(first_view.get()->children[2], 1));
  EXPECT_EQ(shaped->runtime->rows_truncated, 0);
  auto second_batch = readBatch(shaped->stream);
  auto second_view = test::bindArrayView(schema.get(), second_batch.get());
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(second_view.get()->children[1], 0), 4.0);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(second_view.get()->children[2], 0), 5.0);
  EXPECT_EQ(shaped->runtime->rows_truncated, 1);
  EXPECT_EQ(shaped->runtime->first_truncated_column, "ranges");
}

/// Fixed-size lists expand at nested flatten depth while complex-element lists remain dropped.
TEST(TableShaperTest, ExpandsNestedFixedSizeListAndDropsStructList) {
  auto shaped = shapeStream(decodeFixture("lists_nested.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 1);
  EXPECT_EQ(shaped->dropped_columns[0].name, "bad");
  EXPECT_EQ(shaped->dropped_columns[0].format, "+l");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 4);
  EXPECT_STREQ(schema.get()->children[2]->name, "pose/vel[1]");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[2], 0), 2.0);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[2], 1), 5.0);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[2], 2));
}

/// Clamp policy keeps the configured prefix of an oversized list.
TEST(TableShaperTest, ClampsWideListToArrayLimit) {
  ShapeOptions options;
  options.array_limit.max_size = 4;
  auto shaped = shapeStream(decodeFixture("lists_wide.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_TRUE(shaped->dropped_columns.empty());
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 6);
  EXPECT_STREQ(schema.get()->children[4]->name, "wide[3]");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[4], 0), 4.0);
  ASSERT_NE(shaped->runtime, nullptr);
  EXPECT_EQ(shaped->runtime->rows_truncated, 1);
  EXPECT_EQ(shaped->runtime->first_truncated_column, "wide");
}

/// Skip policy removes an oversized list as one dropped source column.
TEST(TableShaperTest, SkipsWideListAtArrayLimit) {
  ShapeOptions options;
  options.array_limit.max_size = 4;
  options.array_limit.policy = PJ::sdk::ArrayPolicy::kSkip;
  auto shaped = shapeStream(decodeFixture("lists_wide.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 1);
  EXPECT_EQ(shaped->dropped_columns[0].name, "wide");
  EXPECT_EQ(shaped->dropped_columns[0].format, "+l");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 2);
  EXPECT_STREQ(schema.get()->children[1]->name, "value");
}

/// An all-null/empty first batch fixes a variable list at width zero and reports it as empty.
TEST(TableShaperTest, DropsVariableListWhenFirstBatchWidthIsZero) {
  auto shaped = shapeStream(decodeFixture("lists_empty_first_batch.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_NE(shaped->runtime, nullptr);
  ASSERT_EQ(shaped->dropped_columns.size(), 1);
  EXPECT_EQ(shaped->dropped_columns[0].name, "empty");
  EXPECT_EQ(shaped->dropped_columns[0].format, "+l(empty)");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 2);
  EXPECT_STREQ(schema.get()->children[1]->name, "value");
  EXPECT_EQ(readBatch(shaped->stream).get()->length, 2);
  EXPECT_EQ(readBatch(shaped->stream).get()->length, 1);
  EXPECT_EQ(shaped->runtime->rows_truncated, 0);
  EXPECT_TRUE(shaped->runtime->first_truncated_column.empty());
}

/// Large lists use 64-bit offsets and timestamp/large-string elements reuse scalar normalization.
TEST(TableShaperTest, ExpandsLargeListsAndNormalizesElementTypes) {
  auto shaped = shapeStream(decodeFixture("lists_normalized_elements.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_TRUE(shaped->dropped_columns.empty());
  auto schema = readSchema(shaped->stream);
  const std::vector<std::string_view> expected_names = {"timestamp_ns",   "large_values[0]", "large_values[1]",
                                                        "typed_times[0]", "typed_times[1]",  "large_names[0]",
                                                        "large_names[1]"};
  ASSERT_EQ(schema.get()->n_children, static_cast<int64_t>(expected_names.size()));
  for (std::size_t index = 0; index < expected_names.size(); ++index) {
    EXPECT_EQ(schema.get()->children[index]->name, expected_names[index]);
  }
  EXPECT_STREQ(schema.get()->children[1]->format, "s");
  EXPECT_STREQ(schema.get()->children[3]->format, "l");
  EXPECT_STREQ(schema.get()->children[5]->format, "u");

  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[1], 0), 10);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[2], 1));
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[3], 0), 1000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[4], 0), 2000);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[4], 1));
  const auto large_name = ArrowArrayViewGetStringUnsafe(view.get()->children[6], 0);
  EXPECT_EQ(std::string_view(large_name.data, static_cast<std::size_t>(large_name.size_bytes)), "a large string value");
  for (int64_t child = 1; child < schema.get()->n_children; ++child) {
    EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[child], 2));
  }
}

/// String-view elements normalize to utf8 in fixed-size list output schemas.
TEST(TableShaperTest, NormalizesStringViewListElementSchema) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 3), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[0], "timestamp_ns"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetTypeFixedSize(schema.get()->children[1], NANOARROW_TYPE_FIXED_SIZE_LIST, 1), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[1], "labels"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1]->children[0], NANOARROW_TYPE_STRING_VIEW), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[2], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[2], "value"), NANOARROW_OK);
  ArrowArrayStream raw_stream{};
  ASSERT_EQ(ArrowBasicArrayStreamInit(&raw_stream, schema.get(), 0), NANOARROW_OK);

  auto shaped = shapeStream(PJ::sdk::ArrowStreamHolder(raw_stream), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto output_schema = readSchema(shaped->stream);
  ASSERT_EQ(output_schema.get()->n_children, 3);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "labels[0]");
  EXPECT_STREQ(output_schema.get()->children[1]->format, "u");
}

/// An explicitly selected narrow integer axis remains usable and announces its limited range.
TEST(TableShaperTest, WidensInt32TimestampAxis) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto shaped = shapeStream(decodeFixture("axis_int32.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  const auto* warning = warningWithCode(*shaped, "parser_arrow.narrow_timestamp_axis");
  ASSERT_NE(warning, nullptr);
  EXPECT_EQ(warning->message, "explicit timestamp column 'time': int32 can express at most 2147483647 ns since epoch");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 3);
}

/// Explicit int16 axes still shape even though automatic detection considers them implausibly narrow.
TEST(TableShaperTest, WidensInt16TimestampAxis) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto shaped = shapeStream(makeIntegerAxisStream(NANOARROW_TYPE_INT16, {-3, 7, 32000}), options);
  ASSERT_TRUE(shaped) << shaped.error();
  const auto* warning = warningWithCode(*shaped, "parser_arrow.narrow_timestamp_axis");
  ASSERT_NE(warning, nullptr);
  EXPECT_EQ(warning->message, "explicit timestamp column 'time': int16 can express at most 32767 ns since epoch");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), -3);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 32000);
}

/// Explicit uint8 axes still shape and surface their maximum expressible instant.
TEST(TableShaperTest, WidensUint8TimestampAxis) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto shaped = shapeStream(makeIntegerAxisStream(NANOARROW_TYPE_UINT8, {0, 200, 255}), options);
  ASSERT_TRUE(shaped) << shaped.error();
  const auto* warning = warningWithCode(*shaped, "parser_arrow.narrow_timestamp_axis");
  ASSERT_NE(warning, nullptr);
  EXPECT_EQ(warning->message, "explicit timestamp column 'time': uint8 can express at most 255 ns since epoch");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), 200);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 255);
}

/// Explicit uint32 timestamp axes are widened while retaining their range warning.
TEST(TableShaperTest, WidensUint32TimestampAxis) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto shaped = shapeStream(decodeFixture("axis_uint32.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  const auto* warning = warningWithCode(*shaped, "parser_arrow.narrow_timestamp_axis");
  ASSERT_NE(warning, nullptr);
  EXPECT_EQ(warning->message, "explicit timestamp column 'time': uint32 can express at most 4294967295 ns since epoch");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), 2);
}

/// Only explicitly selected types outside the plausible set produce a narrow-axis warning.
TEST(TableShaperTest, WarnsForExactlyTheExplicitNarrowAxisTypes) {
  struct AxisCase {
    ArrowType type;
    bool warns;
    std::string_view limitation;
  };
  constexpr std::array<AxisCase, 10> kCases = {{
      {NANOARROW_TYPE_INT8, true, "int8 can express at most 127 ns since epoch"},
      {NANOARROW_TYPE_INT16, true, "int16 can express at most 32767 ns since epoch"},
      {NANOARROW_TYPE_INT32, true, "int32 can express at most 2147483647 ns since epoch"},
      {NANOARROW_TYPE_INT64, false, {}},
      {NANOARROW_TYPE_UINT8, true, "uint8 can express at most 255 ns since epoch"},
      {NANOARROW_TYPE_UINT16, true, "uint16 can express at most 65535 ns since epoch"},
      {NANOARROW_TYPE_UINT32, true, "uint32 can express at most 4294967295 ns since epoch"},
      {NANOARROW_TYPE_UINT64, false, {}},
      {NANOARROW_TYPE_FLOAT, true,
       "float has sub-second resolution only for magnitudes below 8388608 seconds since epoch"},
      {NANOARROW_TYPE_DOUBLE, false, {}},
  }};
  ShapeOptions options;
  options.timestamp_column = "time";
  for (const auto& axis_case : kCases) {
    SCOPED_TRACE(ArrowTypeString(axis_case.type));
    auto schema = makeSchema({{"time", axis_case.type}, {"value", NANOARROW_TYPE_DOUBLE}});
    auto shaped = shapeStream(emptyStream(schema.get()), options);
    ASSERT_TRUE(shaped) << shaped.error();
    const auto* warning = warningWithCode(*shaped, "parser_arrow.narrow_timestamp_axis");
    EXPECT_EQ(warning != nullptr, axis_case.warns);
    if (warning != nullptr) {
      EXPECT_EQ(warning->message, "explicit timestamp column 'time': " + std::string(axis_case.limitation));
    }
    EXPECT_EQ(shaped->warnings.size(), axis_case.warns ? 1U : 0U);
  }

  PJ::sdk::ArrowSchemaHolder timestamp_schema;
  ArrowSchemaInit(timestamp_schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(timestamp_schema.get(), 2), NANOARROW_OK);
  ASSERT_EQ(
      ArrowSchemaSetTypeDateTime(
          timestamp_schema.get()->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_NANO, nullptr),
      NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(timestamp_schema.get()->children[0], "time"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(timestamp_schema.get()->children[1], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(timestamp_schema.get()->children[1], "value"), NANOARROW_OK);
  auto shaped = shapeStream(emptyStream(timestamp_schema.get()), options);
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_TRUE(shaped->warnings.empty());
}

/// Half-float remains unsupported even when the column is selected explicitly.
TEST(TableShaperTest, RejectsExplicitHalfFloatTimestampAxis) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto schema = makeSchema({{"time", NANOARROW_TYPE_HALF_FLOAT}, {"value", NANOARROW_TYPE_DOUBLE}});
  auto shaped = shapeStream(emptyStream(schema.get()), options);
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("unsupported type"), std::string::npos);
}

/// Floating timestamp axes represent seconds and are rounded after conversion to nanoseconds.
TEST(TableShaperTest, ConvertsDoubleSecondsTimestampAxis) {
  auto shaped = shapeStream(decodeFixture("axis_double.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 1'500'000'000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), -500'000'000);
}

/// Float timestamp axes use the same seconds-to-rounded-nanoseconds contract.
TEST(TableShaperTest, ConvertsFloatSecondsTimestampAxis) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto shaped = shapeStream(decodeFixture("axis_float.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  const auto* warning = warningWithCode(*shaped, "parser_arrow.narrow_timestamp_axis");
  ASSERT_NE(warning, nullptr);
  EXPECT_EQ(
      warning->message,
      "explicit timestamp column 'time': float has sub-second resolution only for magnitudes below 8388608 seconds "
      "since epoch");
  auto schema = readSchema(shaped->stream);
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 1'500'000'000);
}

/// Float32 epoch-scale inputs collapse before conversion and record the precision-risk column during the drain.
TEST(TableShaperTest, RecordsFloat32AxisMagnitudeAndCollapsedTimestamps) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto shaped = shapeStream(makeFloatingAxisStream(NANOARROW_TYPE_FLOAT, {1.7e9, 1.7e9 + 0.5, 1.7e9 + 1.0}), options);
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_NE(shaped->runtime, nullptr);
  EXPECT_FALSE(shaped->runtime->float_axis_magnitude_exceeded);

  auto schema = readSchema(shaped->stream);
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  const int64_t collapsed = ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0);
  EXPECT_EQ(collapsed, 1'700'000'000'000'000'000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), collapsed);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), collapsed);
  EXPECT_TRUE(shaped->runtime->float_axis_magnitude_exceeded);
  EXPECT_EQ(shaped->runtime->float_axis_column, "time");
}

/// The float32 precision diagnostic begins at exactly 2^23 seconds, where spacing reaches one second.
TEST(TableShaperTest, DetectsFloat32AxisMagnitudeAtExactBoundary) {
  ShapeOptions options;
  options.timestamp_column = "time";

  auto below = shapeStream(makeFloatingAxisStream(NANOARROW_TYPE_FLOAT, {8'388'607.0}), options);
  ASSERT_TRUE(below) << below.error();
  ASSERT_NE(below->runtime, nullptr);
  EXPECT_EQ(readBatch(below->stream).get()->length, 1);
  EXPECT_FALSE(below->runtime->float_axis_magnitude_exceeded);
  EXPECT_TRUE(below->runtime->float_axis_column.empty());

  auto boundary = shapeStream(makeFloatingAxisStream(NANOARROW_TYPE_FLOAT, {8'388'608.0}), options);
  ASSERT_TRUE(boundary) << boundary.error();
  ASSERT_NE(boundary->runtime, nullptr);
  EXPECT_EQ(readBatch(boundary->stream).get()->length, 1);
  EXPECT_TRUE(boundary->runtime->float_axis_magnitude_exceeded);
  EXPECT_EQ(boundary->runtime->float_axis_column, "time");

  auto negative_boundary = shapeStream(makeFloatingAxisStream(NANOARROW_TYPE_FLOAT, {-8'388'608.0}), options);
  ASSERT_TRUE(negative_boundary) << negative_boundary.error();
  ASSERT_NE(negative_boundary->runtime, nullptr);
  EXPECT_EQ(readBatch(negative_boundary->stream).get()->length, 1);
  EXPECT_TRUE(negative_boundary->runtime->float_axis_magnitude_exceeded);
  EXPECT_EQ(negative_boundary->runtime->float_axis_column, "time");
}

/// Fractional nanoseconds are rounded symmetrically instead of truncated.
TEST(TableShaperTest, RoundsFractionalNanosecondTimestampAxis) {
  auto shaped = shapeStream(decodeFixture("axis_fractional_ns.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 2);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), -2);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 2);
}

/// Splitting integral and fractional seconds preserves an exactly representable fraction at epoch scale.
TEST(TableShaperTest, ConvertsFloatingSecondsWithPortableIntegerArithmetic) {
  int64_t converted = 0;
  ASSERT_TRUE(test::floatingSecondsToNanosecondsForTesting(1'700'000'000.125, &converted));
  EXPECT_EQ(converted, 1'700'000'000'125'000'000);
  ASSERT_TRUE(test::floatingSecondsToNanosecondsForTesting(-1.6e-9, &converted));
  EXPECT_EQ(converted, -2);
}

/// Non-finite floating timestamp seconds fail instead of invoking an invalid integer conversion.
TEST(TableShaperTest, RejectsNonFiniteFloatingTimestampAxis) {
  auto shaped = shapeStream(decodeFixture("axis_double_nonfinite.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), ERANGE);
  EXPECT_NE(
      std::string(ArrowArrayStreamGetLastError(shaped->stream.get())).find("cannot be represented"), std::string::npos);
}

/// Timestamp unit multiplication reports overflow instead of wrapping.
TEST(TableShaperTest, RejectsTypedTimestampScalingOverflow) {
  auto shaped = shapeStream(decodeFixture("timestamp_seconds_overflow.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), ERANGE);
  EXPECT_NE(std::string(ArrowArrayStreamGetLastError(shaped->stream.get())).find("overflows"), std::string::npos);
}

/// A later batch failure replaces successful-callback state with a fresh diagnostic naming the failing column.
TEST(TableShaperTest, ReportsFreshColumnErrorWhenSecondBatchFails) {
  ShapeOptions options;
  options.timestamp_column = "timestamp_ns";
  auto shaped = shapeStream(makeSecondBatchTimestampOverflowStream(), options);
  ASSERT_TRUE(shaped) << shaped.error();
  auto first_batch = readBatch(shaped->stream);
  ASSERT_EQ(first_batch.get()->length, 1);
  EXPECT_STREQ(ArrowArrayStreamGetLastError(shaped->stream.get()), "");

  PJ::sdk::ArrowArrayHolder second_batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), second_batch.out()), ERANGE);
  const char* diagnostic = ArrowArrayStreamGetLastError(shaped->stream.get());
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_FALSE(std::string_view(diagnostic).empty());
  EXPECT_NE(std::string_view(diagnostic).find("delayed_time"), std::string_view::npos);
}

/// Expanded timestamp lists retain column-major conversion precedence while sharing cached row bounds.
TEST(TableShaperTest, ReportsFirstExpandedColumnWhenTimestampListOverflowsCompete) {
  auto shaped = shapeStream(makeCompetingTimestampListOverflowStream(), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), ERANGE);
  EXPECT_STREQ(
      ArrowArrayStreamGetLastError(shaped->stream.get()),
      "parser_arrow: timestamp column 'times[0]' overflows int64 nanoseconds");
}

/// Unsigned 64-bit timestamp ticks outside int64 fail instead of changing sign.
TEST(TableShaperTest, RejectsUint64TimestampAboveInt64Max) {
  auto shaped = shapeStream(decodeFixture("axis_uint64_overflow.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), ERANGE);
  EXPECT_STREQ(
      ArrowArrayStreamGetLastError(shaped->stream.get()), "parser_arrow: timestamp column 'time' exceeds INT64_MAX");
}

/// Unsupported explicitly selected timestamp types are rejected before a shaped schema is exposed.
TEST(TableShaperTest, RejectsStringTimestampAxisDuringPlanning) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto schema = makeSchema({{"time", NANOARROW_TYPE_STRING}, {"value", NANOARROW_TYPE_DOUBLE}});
  auto shaped = shapeStream(emptyStream(schema.get()), options);
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("timestamp column 'time' has unsupported type 'u'"), std::string::npos);
}

/// Null timestamp cells fail get_next and poison the lazy wrapper instead of exposing garbage.
TEST(TableShaperTest, RejectsNullTimestampCellAndPoisonsStream) {
  auto shaped = shapeStream(decodeFixture("axis_null.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  PJ::sdk::ArrowArrayHolder batch;
  const int first_result = shaped->stream.get()->get_next(shaped->stream.get(), batch.out());
  ASSERT_NE(first_result, NANOARROW_OK);
  EXPECT_NE(std::string(ArrowArrayStreamGetLastError(shaped->stream.get())).find("null"), std::string::npos);
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), first_result);
}

/// Schema reads cannot erase the code or diagnostic retained by a poisoned get_next callback.
TEST(TableShaperTest, PreservesPoisonedErrorAcrossSchemaRead) {
  auto shaped = shapeStream(decodeFixture("axis_null.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();

  PJ::sdk::ArrowArrayHolder first_batch;
  const int first_result = shaped->stream.get()->get_next(shaped->stream.get(), first_batch.out());
  ASSERT_NE(first_result, NANOARROW_OK);
  const char* first_diagnostic = ArrowArrayStreamGetLastError(shaped->stream.get());
  ASSERT_NE(first_diagnostic, nullptr);
  const std::string expected_diagnostic(first_diagnostic);
  ASSERT_FALSE(expected_diagnostic.empty());

  PJ::sdk::ArrowSchemaHolder schema;
  EXPECT_EQ(shaped->stream.get()->get_schema(shaped->stream.get(), schema.out()), NANOARROW_OK);
  EXPECT_EQ(ArrowArrayStreamGetLastError(shaped->stream.get()), expected_diagnostic);

  PJ::sdk::ArrowArrayHolder repeated_batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), repeated_batch.out()), first_result);
  EXPECT_EQ(ArrowArrayStreamGetLastError(shaped->stream.get()), expected_diagnostic);
}

/// Axis null validation wins over an earlier overflowing value in the same timestamp column.
TEST(TableShaperTest, ReportsAxisNullBeforeEarlierAxisOverflow) {
  auto shaped = shapeStream(makeAxisNullPrecedenceStream(false), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), EINVAL);
  EXPECT_STREQ(
      ArrowArrayStreamGetLastError(shaped->stream.get()),
      "parser_arrow: timestamp column 'time' contains a null value");
}

/// Axis null validation also wins over a preceding non-axis timestamp conversion failure.
TEST(TableShaperTest, ReportsLaterAxisNullBeforeEarlierDataTimestampOverflow) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto shaped = shapeStream(makeAxisNullPrecedenceStream(true), options);
  ASSERT_TRUE(shaped) << shaped.error();
  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), EINVAL);
  EXPECT_STREQ(
      ArrowArrayStreamGetLastError(shaped->stream.get()),
      "parser_arrow: timestamp column 'time' contains a null value");
}

/// Host-skipped final output columns are reported and absent from the shaped schema.
TEST(TableShaperTest, ReportsDroppedOutputColumns) {
  auto schema =
      makeSchema({{"timestamp_ns", NANOARROW_TYPE_INT64}, {"a", NANOARROW_TYPE_LIST}, {"b", NANOARROW_TYPE_DOUBLE}});
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get()->children[1]->children[0], 1), NANOARROW_OK);
  ASSERT_EQ(
      ArrowSchemaSetType(schema.get()->children[1]->children[0]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 1);
  EXPECT_EQ(shaped->dropped_columns[0].name, "a");
  EXPECT_EQ(shaped->dropped_columns[0].format, "+l");
  auto output_schema = readSchema(shaped->stream);
  ASSERT_EQ(output_schema.get()->n_children, 2);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "b");
}

/// Truncated dropped-column details carry their own ellipsis so every caller applies identical punctuation.
TEST(TableShaperTest, FormatsDroppedColumnsWithEllipsisWhenTruncated) {
  std::vector<DroppedColumn> columns;
  columns.reserve(kMaxDroppedColumnsListed + 1);
  for (std::size_t index = 0; index <= kMaxDroppedColumnsListed; ++index) {
    columns.push_back(DroppedColumn{"column_" + std::to_string(index), "i"});
  }

  const std::string formatted = formatDroppedColumns(columns, kMaxDroppedColumnsListed);
  EXPECT_TRUE(formatted.ends_with(", …"));
  EXPECT_EQ(formatted.find("column_" + std::to_string(kMaxDroppedColumnsListed)), std::string::npos);
}

/// Lists with nested-list or binary elements remain on the dropped-column path.
TEST(TableShaperTest, ReportsNonIngestibleListElementTypesAsDropped) {
  auto schema = makeSchema(
      {{"timestamp_ns", NANOARROW_TYPE_INT64},
       {"nested", NANOARROW_TYPE_LIST},
       {"binary", NANOARROW_TYPE_LIST},
       {"dictionary", NANOARROW_TYPE_LIST},
       {"value", NANOARROW_TYPE_DOUBLE}});
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1]->children[0], NANOARROW_TYPE_LIST), NANOARROW_OK);
  ASSERT_EQ(
      ArrowSchemaSetType(schema.get()->children[1]->children[0]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[2]->children[0], NANOARROW_TYPE_BINARY), NANOARROW_OK);
  PJ::sdk::ArrowSchemaHolder dictionary_element;
  ASSERT_EQ(ArrowSchemaInitFromType(dictionary_element.out(), NANOARROW_TYPE_INT8), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaAllocateDictionary(dictionary_element.get()), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaInitFromType(dictionary_element.get()->dictionary, NANOARROW_TYPE_STRING), NANOARROW_OK);
  schema.get()->children[3]->children[0]->release(schema.get()->children[3]->children[0]);
  ArrowSchemaMove(dictionary_element.get(), schema.get()->children[3]->children[0]);
  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 3);
  EXPECT_EQ(shaped->dropped_columns[0].name, "nested");
  EXPECT_EQ(shaped->dropped_columns[0].format, "+l");
  EXPECT_EQ(shaped->dropped_columns[1].name, "binary");
  EXPECT_EQ(shaped->dropped_columns[1].format, "+l");
  EXPECT_EQ(shaped->dropped_columns[2].name, "dictionary");
  EXPECT_EQ(shaped->dropped_columns[2].format, "+l");
  auto output_schema = readSchema(shaped->stream);
  ASSERT_EQ(output_schema.get()->n_children, 2);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "value");
}

/// Duplicate names remain errors even when one colliding column would otherwise be dropped by host compatibility.
TEST(TableShaperTest, RejectsDuplicateNameSharedByDroppedAndRetainedColumns) {
  auto schema = makeSchema(
      {{"timestamp_ns", NANOARROW_TYPE_INT64},
       {"duplicate", NANOARROW_TYPE_BINARY},
       {"duplicate", NANOARROW_TYPE_DOUBLE},
       {"value", NANOARROW_TYPE_DOUBLE}});
  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("duplicate output column name 'duplicate'"), std::string::npos);
}

/// Deferred empty-list drops retain source order relative to later immediately unsupported columns.
TEST(TableShaperTest, PreservesSourceOrderAcrossDeferredAndImmediateDrops) {
  auto schema = makeSchema(
      {{"timestamp_ns", NANOARROW_TYPE_INT64},
       {"empty", NANOARROW_TYPE_LIST},
       {"binary", NANOARROW_TYPE_BINARY},
       {"value", NANOARROW_TYPE_DOUBLE}});
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1]->children[0], NANOARROW_TYPE_INT32), NANOARROW_OK);

  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 2);
  EXPECT_EQ(shaped->dropped_columns[0].name, "empty");
  EXPECT_EQ(shaped->dropped_columns[0].format, "+l(empty)");
  EXPECT_EQ(shaped->dropped_columns[1].name, "binary");
  EXPECT_EQ(shaped->dropped_columns[1].format, "z");
}

/// Nullable unsupported scalar leaves are removed while supported siblings retain their values.
TEST(TableShaperTest, RemovesDroppedScalarLeavesUnderNullableStruct) {
  auto shaped = shapeStream(decodeFixture("nested_dropped_scalars.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 2);
  EXPECT_EQ(shaped->dropped_columns[0].name, "metadata/date");
  EXPECT_EQ(shaped->dropped_columns[0].format, "tdD");
  EXPECT_EQ(shaped->dropped_columns[1].name, "metadata/amount");
  EXPECT_EQ(shaped->dropped_columns[1].format, "d:10,2");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 2);
  EXPECT_STREQ(schema.get()->children[0]->name, "timestamp_ns");
  EXPECT_STREQ(schema.get()->children[1]->name, "value");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 0), 1.0);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 1), 2.0);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 2), 3.0);
}

/// A primitive list under a nullable struct expands and inherits parent nulls.
TEST(TableShaperTest, ExpandsListUnderNullableStructAndPropagatesParentNull) {
  auto shaped = shapeStream(decodeFixture("nullable_struct_list.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_TRUE(shaped->dropped_columns.empty());

  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 6);
  EXPECT_STREQ(schema.get()->children[0]->name, "timestamp_ns");
  EXPECT_STREQ(schema.get()->children[1]->name, "value");
  EXPECT_STREQ(schema.get()->children[2]->name, "metadata/samples[0]");
  EXPECT_STREQ(schema.get()->children[4]->name, "metadata/samples[2]");
  EXPECT_STREQ(schema.get()->children[5]->name, "metadata/note");

  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  ASSERT_EQ(batch.get()->length, 3);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 1000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 3000);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 0), 1.5);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 2), 3.5);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[2], 0), 1);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[4], 0));
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[2], 1));
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[3], 1));
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[4], 1));
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[4], 2), 5);
  const ArrowStringView first_note = ArrowArrayViewGetStringUnsafe(view.get()->children[5], 0);
  EXPECT_EQ(std::string_view(first_note.data, static_cast<std::size_t>(first_note.size_bytes)), "first");
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[5], 1));
  const ArrowStringView third_note = ArrowArrayViewGetStringUnsafe(view.get()->children[5], 2);
  EXPECT_EQ(std::string_view(third_note.data, static_cast<std::size_t>(third_note.size_bytes)), "third");
}

/// A schema with no host-ingestible data produces an actionable planning error.
TEST(TableShaperTest, RejectsSchemaWhenAllDataColumnsWouldBeDropped) {
  auto schema = makeSchema({{"timestamp_ns", NANOARROW_TYPE_INT64}, {"a", NANOARROW_TYPE_LIST}});
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get()->children[1]->children[0], 1), NANOARROW_OK);
  ASSERT_EQ(
      ArrowSchemaSetType(schema.get()->children[1]->children[0]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("no host-ingestible columns"), std::string::npos);
  EXPECT_NE(shaped.error().find("a:+l"), std::string::npos);
}

/// Flattening rejects output names that collide with an already-flat field.
TEST(TableShaperTest, RejectsDuplicateOutputNames) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 4), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[0], "timestamp_ns"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get()->children[1], 1), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[1], "pose"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[1]->children[0], "x"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[2], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[2], "pose/x"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[3], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[3], "value"), NANOARROW_OK);
  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("duplicate output column name 'pose/x'"), std::string::npos);
}

/// Fixed-size list widths are exposed in the shaped schema and participate in duplicate-name checks.
TEST(TableShaperTest, PlansFixedSizeListFromSchemaAndRejectsExpandedNameCollision) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 4), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[0], "timestamp_ns"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetTypeFixedSize(schema.get()->children[1], NANOARROW_TYPE_FIXED_SIZE_LIST, 3), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[1], "a"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[2], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[2], "value"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[3], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[3], "spare"), NANOARROW_OK);

  auto shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto output_schema = readSchema(shaped->stream);
  ASSERT_EQ(output_schema.get()->n_children, 6);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "a[0]");
  EXPECT_STREQ(output_schema.get()->children[3]->name, "a[2]");
  EXPECT_STREQ(output_schema.get()->children[4]->name, "value");

  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[3], "a[0]"), NANOARROW_OK);
  shaped = shapeStream(emptyStream(schema.get()), ShapeOptions{});
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("duplicate output column name 'a[0]'"), std::string::npos);
}

/// A variable list's peek-resolved element names use the same duplicate-name error.
TEST(TableShaperTest, RejectsVariableListExpansionNameCollisionAfterPeek) {
  auto shaped = shapeStream(decodeFixture("lists_collision.arrows"), ShapeOptions{});
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("duplicate output column name 'a[0]'"), std::string::npos);
}

/// Errors encountered while peeking a variable list's first batch fail shapeStream itself.
TEST(TableShaperTest, ReportsFirstBatchPeekErrorDuringShapeStream) {
  auto schema = makeSchema(
      {{"timestamp_ns", NANOARROW_TYPE_INT64}, {"a", NANOARROW_TYPE_LIST}, {"value", NANOARROW_TYPE_DOUBLE}});
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  auto shaped = shapeStream(makeFailingPeekStream(std::move(schema)), ShapeOptions{});
  ASSERT_FALSE(shaped);
  EXPECT_NE(shaped.error().find("configured first-batch failure"), std::string::npos);
}

/// Empty field names are replaced with stable child-index placeholders.
TEST(TableShaperTest, SubstitutesEmptyOutputNames) {
  auto schema = makeSchema({{"timestamp_ns", NANOARROW_TYPE_INT64}, {"", NANOARROW_TYPE_DOUBLE}});
  ArrowArrayStream raw_stream{};
  ASSERT_EQ(ArrowBasicArrayStreamInit(&raw_stream, schema.get(), 0), NANOARROW_OK);
  auto shaped = shapeStream(PJ::sdk::ArrowStreamHolder(raw_stream), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto output_schema = readSchema(shaped->stream);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "_1");
}

/// A genuinely null Arrow child name uses the same stable index fallback.
TEST(TableShaperTest, SubstitutesNullOutputNames) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 2), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[0], "timestamp_ns"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(schema.get()->children[1]->name, nullptr);
  ArrowArrayStream raw_stream{};
  ASSERT_EQ(ArrowBasicArrayStreamInit(&raw_stream, schema.get(), 0), NANOARROW_OK);
  auto shaped = shapeStream(PJ::sdk::ArrowStreamHolder(raw_stream), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto output_schema = readSchema(shaped->stream);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "_1");
}

/// Nested empty names use their child index after the sanitized parent path.
TEST(TableShaperTest, SubstitutesNestedEmptyOutputNames) {
  PJ::sdk::ArrowSchemaHolder schema;
  ArrowSchemaInit(schema.out());
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 3), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[0], NANOARROW_TYPE_INT64), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[0], "timestamp_ns"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get()->children[1], 1), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[1], "parent"), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[1]->children[0], ""), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[2], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  ASSERT_EQ(ArrowSchemaSetName(schema.get()->children[2], "value"), NANOARROW_OK);
  ArrowArrayStream raw_stream{};
  ASSERT_EQ(ArrowBasicArrayStreamInit(&raw_stream, schema.get(), 0), NANOARROW_OK);
  auto shaped = shapeStream(PJ::sdk::ArrowStreamHolder(raw_stream), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto output_schema = readSchema(shaped->stream);
  EXPECT_STREQ(output_schema.get()->children[1]->name, "parent/_0");
}

/// A flat supported stream preserves its observable schema and values through the always-wrapped shaper.
TEST(TableShaperTest, PlansPassThroughWhenNothingApplies) {
  auto shaped = shapeStream(decodeFixture("flat.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "timestamp_ns");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 4);
  EXPECT_STREQ(schema.get()->children[0]->name, "timestamp_ns");
  EXPECT_STREQ(schema.get()->children[1]->name, "a");
  EXPECT_STREQ(schema.get()->children[2]->name, "b");
  EXPECT_STREQ(schema.get()->children[3]->name, "name");
  auto batch = readBatch(shaped->stream);
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 3000);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 1), 2.5);
}

}  // namespace
}  // namespace pj::parser_arrow
