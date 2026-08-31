#include "table_shaper.hpp"

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ipc_decoder.hpp"
#include "pj_base/sdk/arrow.hpp"
#include "pj_base/span.hpp"
#include "test_utils.hpp"

#ifndef PJ_ARROW_TEST_DATA_DIR
#error "PJ_ARROW_TEST_DATA_DIR must be defined"
#endif

namespace pj::parser_arrow {
namespace {

/// Return the absolute path of a checked-in parser_arrow fixture.
[[nodiscard]] std::filesystem::path fixturePath(std::string_view filename) {
  return std::filesystem::path(PJ_ARROW_TEST_DATA_DIR) / filename;
}

/// Decode one checked-in IPC fixture or throw with its parser diagnostic.
[[nodiscard]] PJ::sdk::ArrowStreamHolder decodeFixture(std::string_view filename) {
  const auto bytes = test::readFile(fixturePath(filename));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  if (!decoded) {
    throw std::runtime_error(decoded.error());
  }
  return std::move(*decoded);
}

/// Read a stream schema or throw its stream diagnostic.
[[nodiscard]] PJ::sdk::ArrowSchemaHolder readSchema(PJ::sdk::ArrowStreamHolder& stream) {
  PJ::sdk::ArrowSchemaHolder schema;
  const int result = stream.get()->get_schema(stream.get(), schema.out());
  if (result != NANOARROW_OK) {
    throw std::runtime_error(ArrowArrayStreamGetLastError(stream.get()));
  }
  return schema;
}

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

/// Initialize and bind an ArrowArrayView for one schema and batch.
class ScopedArrayView {
 public:
  /// Bind an array view or throw nanoarrow's validation diagnostic.
  ScopedArrayView(const ArrowSchema* schema, const ArrowArray* array) {
    ArrowError error{};
    const int init_result = ArrowArrayViewInitFromSchema(&view_, schema, &error);
    if (init_result != NANOARROW_OK) {
      throw std::runtime_error(error.message);
    }
    const int bind_result = ArrowArrayViewSetArray(&view_, array, &error);
    if (bind_result != NANOARROW_OK) {
      ArrowArrayViewReset(&view_);
      throw std::runtime_error(error.message);
    }
  }

  ScopedArrayView(const ScopedArrayView&) = delete;
  ScopedArrayView& operator=(const ScopedArrayView&) = delete;

  /// Release the recursively allocated nanoarrow view.
  ~ScopedArrayView() {
    ArrowArrayViewReset(&view_);
  }

  /// Access the bound view.
  [[nodiscard]] const ArrowArrayView* get() const {
    return &view_;
  }

 private:
  ArrowArrayView view_{};
};

/// Pull the next shaped batch and require that it is not end-of-stream.
[[nodiscard]] PJ::sdk::ArrowArrayHolder readBatch(PJ::sdk::ArrowStreamHolder& stream) {
  PJ::sdk::ArrowArrayHolder batch;
  const int result = stream.get()->get_next(stream.get(), batch.out());
  if (result != NANOARROW_OK) {
    throw std::runtime_error(ArrowArrayStreamGetLastError(stream.get()));
  }
  if (!batch.valid()) {
    throw std::runtime_error("unexpected end of stream");
  }
  return batch;
}

/// Re-encode decoded canonical storage as C-Data views because nanoarrow 0.7 cannot decode view IPC batches.
[[nodiscard]] PJ::sdk::ArrowStreamHolder makeViewStream(PJ::sdk::ArrowStreamHolder storage_stream) {
  auto storage_schema = readSchema(storage_stream);
  auto storage_batch = readBatch(storage_stream);
  ScopedArrayView storage_view(storage_schema.get(), storage_batch.get());

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

/// TIMESTAMP fields take precedence over heuristic names.
TEST(TableShaperTest, DetectsTimestampTypeBeforeNamedDecoy) {
  auto stream = decodeFixture("timestamp_typed.arrows");
  auto schema = readSchema(stream);
  EXPECT_EQ(detectTimestampColumn(schema.get()), "stamp");
}

/// Name heuristics use their specified global priority, not schema order.
TEST(TableShaperTest, DetectsTimestampNamesInPriorityOrder) {
  auto with_timestamp =
      makeSchema({{"ts", NANOARROW_TYPE_INT64}, {"time", NANOARROW_TYPE_INT64}, {"timestamp", NANOARROW_TYPE_INT64}});
  EXPECT_EQ(detectTimestampColumn(with_timestamp.get()), "timestamp");

  auto with_time = makeSchema({{"ts", NANOARROW_TYPE_INT64}, {"time", NANOARROW_TYPE_INT64}});
  EXPECT_EQ(detectTimestampColumn(with_time.get()), "time");
}

/// Schemas without typed or named candidates have no detected timestamp.
TEST(TableShaperTest, DetectTimestampReturnsEmptyWithoutCandidate) {
  auto schema = makeSchema({{"a", NANOARROW_TYPE_DOUBLE}, {"b", NANOARROW_TYPE_INT32}});
  EXPECT_TRUE(detectTimestampColumn(schema.get()).empty());
}

/// An explicit existing timestamp field overrides automatic type detection.
TEST(TableShaperTest, UsesExplicitTimestampColumnAsIs) {
  ShapeOptions options;
  options.timestamp_column = "time";
  auto shaped = shapeStream(decodeFixture("timestamp_typed.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "time");
  EXPECT_FALSE(shaped->synthesized_timestamp);
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

/// An empty option selects the detector's result.
TEST(TableShaperTest, AutoDetectsTimestampColumn) {
  auto shaped = shapeStream(decodeFixture("timestamp_typed.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "stamp");
  EXPECT_FALSE(shaped->synthesized_timestamp);
}

/// Missing timestamps produce a prepended int64 timestamp_ns sequence.
TEST(TableShaperTest, SynthesizesTimestampColumnForEveryRow) {
  ShapeOptions options;
  options.message_timestamp_ns = 5000;
  options.synthetic_interval_ns = 7;
  auto shaped = shapeStream(decodeFixture("no_timestamp.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "timestamp_ns");
  EXPECT_TRUE(shaped->synthesized_timestamp);

  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 3);
  EXPECT_STREQ(schema.get()->children[0]->name, "timestamp_ns");
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
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
  ScopedArrayView first_view(schema.get(), first_batch.get());
  ASSERT_EQ(first_batch.get()->length, 2);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(first_view.get()->children[0], 0), 5000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(first_view.get()->children[0], 1), 5007);

  auto second_batch = readBatch(shaped->stream);
  ScopedArrayView second_view(schema.get(), second_batch.get());
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
  ScopedArrayView view(schema.get(), batch.get());
  for (int64_t row = 0; row < batch.get()->length; ++row) {
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], row), 5000);
  }
}

/// Typed nanosecond timestamp axes are exposed to the host as int64 nanoseconds.
TEST(TableShaperTest, CastsTypedTimestampAxisToInt64Nanoseconds) {
  auto shaped = shapeStream(decodeFixture("timestamp_typed.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  EXPECT_EQ(shaped->timestamp_column, "stamp");
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
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
  ScopedArrayView view(schema.get(), batch.get());
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
  ScopedArrayView view(schema.get(), batch.get());
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
  ScopedArrayView view(schema.get(), batch.get());
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
  ScopedArrayView view(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 2000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), 3000);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[1], 0), 4.0);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[1], 1));
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[8], 0), 6.5);
  EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(view.get()->children[8], 1), 7.5);
}

/// Disabling flattening passes the struct column through unchanged.
TEST(TableShaperTest, PreservesStructColumnWhenFlatteningDisabled) {
  ShapeOptions options;
  options.flatten_structs = false;
  auto shaped = shapeStream(decodeFixture("nested.arrows"), options);
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 3);
  EXPECT_STREQ(schema.get()->children[1]->name, "pose");
  EXPECT_STREQ(schema.get()->children[1]->format, "+s");
}

/// String and binary views normalize to canonical arrays with bytes and nulls preserved.
TEST(TableShaperTest, NormalizesStringAndBinaryViews) {
  auto shaped = shapeStream(makeViewStream(decodeFixture("views_storage.arrows")), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 4);
  EXPECT_STREQ(schema.get()->children[1]->format, "u");
  EXPECT_STREQ(schema.get()->children[2]->format, "z");

  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
  const ArrowStringView long_label = ArrowArrayViewGetStringUnsafe(view.get()->children[1], 1);
  EXPECT_EQ(
      std::string_view(long_label.data, static_cast<std::size_t>(long_label.size_bytes)),
      "this string is longer than twelve bytes");
  const ArrowBufferView long_blob = ArrowArrayViewGetBytesUnsafe(view.get()->children[2], 1);
  EXPECT_EQ(
      std::string_view(long_blob.data.as_char, static_cast<std::size_t>(long_blob.size_bytes)), "0123456789abcdef");
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[1], 2));
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[2], 2));
}

/// Canonical-width string and binary formats do not trigger normalization.
TEST(TableShaperTest, LeavesCanonicalVariableWidthColumnsInPlace) {
  auto schema = makeSchema(
      {{"timestamp_ns", NANOARROW_TYPE_INT64},
       {"string", NANOARROW_TYPE_STRING},
       {"binary", NANOARROW_TYPE_BINARY},
       {"value", NANOARROW_TYPE_DOUBLE}});
  auto plan = planShape(schema.get(), ShapeOptions{});
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_FALSE(plan->needs_rewrite);
  ASSERT_EQ(plan->dropped_columns.size(), 1);
  EXPECT_EQ(plan->dropped_columns[0].name, "binary");
  EXPECT_EQ(plan->dropped_columns[0].format, "z");
}

/// Large string/binary offsets are copied into host-compatible canonical-width arrays.
TEST(TableShaperTest, NormalizesLargeStringAndBinaryColumns) {
  auto shaped = shapeStream(decodeFixture("large_types.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[1]->format, "u");
  EXPECT_STREQ(schema.get()->children[2]->format, "z");
  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
  const ArrowStringView label = ArrowArrayViewGetStringUnsafe(view.get()->children[1], 1);
  EXPECT_EQ(std::string_view(label.data, static_cast<std::size_t>(label.size_bytes)), "this is a large string value");
  const ArrowBufferView blob = ArrowArrayViewGetBytesUnsafe(view.get()->children[2], 1);
  EXPECT_EQ(std::string_view(blob.data.as_char, static_cast<std::size_t>(blob.size_bytes)), "large binary value");
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[1], 2));
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[2], 2));
}

/// Narrow integer timestamp axes are widened to int64 without changing units.
TEST(TableShaperTest, WidensInt32TimestampAxis) {
  auto shaped = shapeStream(decodeFixture("axis_int32.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 3);
}

/// Unsigned 32-bit timestamp axes are widened to int64 without changing units.
TEST(TableShaperTest, WidensUint32TimestampAxis) {
  auto shaped = shapeStream(decodeFixture("axis_uint32.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), 2);
}

/// Floating timestamp axes represent seconds and are rounded after conversion to nanoseconds.
TEST(TableShaperTest, ConvertsDoubleSecondsTimestampAxis) {
  auto shaped = shapeStream(decodeFixture("axis_double.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  EXPECT_STREQ(schema.get()->children[0]->format, "l");
  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 1'500'000'000);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), -500'000'000);
}

/// Float timestamp axes use the same seconds-to-rounded-nanoseconds contract.
TEST(TableShaperTest, ConvertsFloatSecondsTimestampAxis) {
  auto shaped = shapeStream(decodeFixture("axis_float.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 1'500'000'000);
}

/// Fractional nanoseconds are rounded symmetrically instead of truncated.
TEST(TableShaperTest, RoundsFractionalNanosecondTimestampAxis) {
  auto shaped = shapeStream(decodeFixture("axis_fractional_ns.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  auto schema = readSchema(shaped->stream);
  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 0), 2);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 1), -2);
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[0], 2), 2);
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

/// Unsigned 64-bit timestamp ticks outside int64 fail instead of changing sign.
TEST(TableShaperTest, RejectsUint64TimestampAboveInt64Max) {
  auto shaped = shapeStream(decodeFixture("axis_uint64_overflow.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  PJ::sdk::ArrowArrayHolder batch;
  EXPECT_EQ(shaped->stream.get()->get_next(shaped->stream.get(), batch.out()), ERANGE);
  EXPECT_NE(std::string(ArrowArrayStreamGetLastError(shaped->stream.get())).find("INT64_MAX"), std::string::npos);
}

/// Unsupported timestamp types are rejected while planning, before any batch is consumed.
TEST(TableShaperTest, RejectsStringTimestampAxisDuringPlanning) {
  auto schema = makeSchema({{"time", NANOARROW_TYPE_STRING}, {"value", NANOARROW_TYPE_DOUBLE}});
  auto plan = planShape(schema.get(), ShapeOptions{});
  ASSERT_FALSE(plan);
  EXPECT_NE(plan.error().find("timestamp column 'time' has unsupported type 'u'"), std::string::npos);
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

/// Host-skipped final output columns are reported while supported data remains ingestible.
TEST(TableShaperTest, ReportsDroppedOutputColumns) {
  auto schema =
      makeSchema({{"timestamp_ns", NANOARROW_TYPE_INT64}, {"a", NANOARROW_TYPE_LIST}, {"b", NANOARROW_TYPE_DOUBLE}});
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  auto plan = planShape(schema.get(), ShapeOptions{});
  ASSERT_TRUE(plan) << plan.error();
  ASSERT_EQ(plan->dropped_columns.size(), 1);
  EXPECT_EQ(plan->dropped_columns[0].name, "a");
  EXPECT_EQ(plan->dropped_columns[0].format, "+l");
}

/// Nullable flattened date/decimal leaves preserve values and parent nulls even though the host skips them.
TEST(TableShaperTest, CopiesDroppedScalarLeavesUnderNullableStruct) {
  auto shaped = shapeStream(decodeFixture("nested_dropped_scalars.arrows"), ShapeOptions{});
  ASSERT_TRUE(shaped) << shaped.error();
  ASSERT_EQ(shaped->dropped_columns.size(), 2);
  EXPECT_EQ(shaped->dropped_columns[0].name, "metadata/date");
  EXPECT_EQ(shaped->dropped_columns[0].format, "tdD");
  EXPECT_EQ(shaped->dropped_columns[1].name, "metadata/amount");
  EXPECT_EQ(shaped->dropped_columns[1].format, "d:10,2");
  auto schema = readSchema(shaped->stream);
  ASSERT_EQ(schema.get()->n_children, 4);
  EXPECT_STREQ(schema.get()->children[1]->name, "metadata/date");
  EXPECT_STREQ(schema.get()->children[2]->name, "metadata/amount");
  auto batch = readBatch(shaped->stream);
  ScopedArrayView view(schema.get(), batch.get());
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[1], 0), 18'263);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[1], 1));
  EXPECT_EQ(ArrowArrayViewGetIntUnsafe(view.get()->children[1], 2), 18'265);
  ArrowDecimal amount{};
  ArrowDecimalInit(&amount, 128, 10, 2);
  ArrowArrayViewGetDecimalUnsafe(view.get()->children[2], 0, &amount);
  EXPECT_EQ(ArrowDecimalGetIntUnsafe(&amount), 1234);
  EXPECT_TRUE(ArrowArrayViewIsNull(view.get()->children[2], 1));
  ArrowArrayViewGetDecimalUnsafe(view.get()->children[2], 2, &amount);
  EXPECT_EQ(ArrowDecimalGetIntUnsafe(&amount), -567);
}

/// A schema with no host-ingestible data produces an actionable planning error.
TEST(TableShaperTest, RejectsSchemaWhenAllDataColumnsWouldBeDropped) {
  auto schema = makeSchema({{"timestamp_ns", NANOARROW_TYPE_INT64}, {"a", NANOARROW_TYPE_LIST}});
  ASSERT_EQ(ArrowSchemaSetType(schema.get()->children[1]->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  auto plan = planShape(schema.get(), ShapeOptions{});
  ASSERT_FALSE(plan);
  EXPECT_NE(plan.error().find("no host-ingestible columns"), std::string::npos);
  EXPECT_NE(plan.error().find("a:+l"), std::string::npos);
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
  auto plan = planShape(schema.get(), ShapeOptions{});
  ASSERT_FALSE(plan);
  EXPECT_NE(plan.error().find("duplicate output column name 'pose/x'"), std::string::npos);
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

/// A flat supported schema exposes an observable no-rewrite pass-through plan.
TEST(TableShaperTest, PlansPassThroughWhenNothingApplies) {
  auto stream = decodeFixture("flat.arrows");
  auto schema = readSchema(stream);
  auto plan = planShape(schema.get(), ShapeOptions{});
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_FALSE(plan->needs_rewrite);
  EXPECT_EQ(plan->timestamp_column, "timestamp_ns");
  EXPECT_FALSE(plan->synthesize_timestamp);
}

}  // namespace
}  // namespace pj::parser_arrow
