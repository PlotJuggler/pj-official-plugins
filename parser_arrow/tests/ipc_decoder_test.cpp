#include "ipc_decoder.hpp"

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/// Return whether text contains "lz4" without regard to ASCII case.
[[nodiscard]] bool containsLz4(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return text.find("lz4") != std::string::npos;
}

/// Verify one flat fixture batch starting at the requested source row.
void verifyFlatBatch(
    const ArrowSchema* schema, const ArrowArray* batch, int64_t source_row_offset, int64_t expected_length) {
  ArrowArrayView array_view{};
  const auto reset_view = [](ArrowArrayView* view) { ArrowArrayViewReset(view); };
  const std::unique_ptr<ArrowArrayView, decltype(reset_view)> view_owner(&array_view, reset_view);
  ArrowError error{};
  ASSERT_EQ(ArrowArrayViewInitFromSchema(&array_view, schema, &error), NANOARROW_OK) << error.message;
  ASSERT_EQ(ArrowArrayViewSetArray(&array_view, batch, &error), NANOARROW_OK) << error.message;
  ASSERT_EQ(batch->length, expected_length);

  constexpr std::array<int64_t, 3> kTimestamps = {1000, 2000, 3000};
  constexpr std::array<double, 3> kAValues = {1.5, 2.5, 3.5};
  constexpr std::array<int64_t, 3> kBValues = {10, 20, 30};
  constexpr std::array<std::string_view, 3> kNames = {"x", "y", "z"};

  for (int64_t batch_row = 0; batch_row < batch->length; ++batch_row) {
    const auto source_row = static_cast<std::size_t>(source_row_offset + batch_row);
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(array_view.children[0], batch_row), kTimestamps[source_row]);
    EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(array_view.children[1], batch_row), kAValues[source_row]);
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(array_view.children[2], batch_row), kBValues[source_row]);

    const ArrowStringView name = ArrowArrayViewGetStringUnsafe(array_view.children[3], batch_row);
    EXPECT_EQ(std::string_view(name.data, static_cast<std::size_t>(name.size_bytes)), kNames[source_row]);
  }
}

/// Verify that get_next reports the Arrow stream end sentinel.
void verifyEndOfStream(ArrowArrayStream* stream) {
  PJ::sdk::ArrowArrayHolder end_of_stream;
  ASSERT_EQ(stream->get_next(stream, end_of_stream.out()), NANOARROW_OK) << ArrowArrayStreamGetLastError(stream);
  EXPECT_FALSE(end_of_stream.valid());
}

/// Flat IPC streams expose the exact schema and one batch of expected values.
TEST(IpcDecoderTest, DecodesFlatStreamSchemaValuesAndEndOfStream) {
  const auto bytes = test::readFile(fixturePath("flat.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto stream = std::move(*decoded);

  PJ::sdk::ArrowSchemaHolder schema;
  ASSERT_EQ(stream.get()->get_schema(stream.get(), schema.out()), NANOARROW_OK)
      << ArrowArrayStreamGetLastError(stream.get());
  ASSERT_EQ(schema.get()->n_children, 4);
  constexpr std::array<std::string_view, 4> kExpectedNames = {"timestamp_ns", "a", "b", "name"};
  constexpr std::array<std::string_view, 4> kExpectedFormats = {"l", "g", "i", "u"};
  for (std::size_t child_index = 0; child_index < kExpectedNames.size(); ++child_index) {
    EXPECT_EQ(schema.get()->children[child_index]->name, kExpectedNames[child_index]);
    EXPECT_EQ(schema.get()->children[child_index]->format, kExpectedFormats[child_index]);
  }

  PJ::sdk::ArrowArrayHolder batch;
  ASSERT_EQ(stream.get()->get_next(stream.get(), batch.out()), NANOARROW_OK)
      << ArrowArrayStreamGetLastError(stream.get());
  ASSERT_TRUE(batch.valid());
  verifyFlatBatch(schema.get(), batch.get(), 0, 3);
  batch.reset();
  verifyEndOfStream(stream.get());
}

/// A stream with two record batches preserves batch boundaries and reaches EOS.
TEST(IpcDecoderTest, IteratesMultipleRecordBatches) {
  const auto bytes = test::readFile(fixturePath("flat_two_batches.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto stream = std::move(*decoded);

  PJ::sdk::ArrowSchemaHolder schema;
  ASSERT_EQ(stream.get()->get_schema(stream.get(), schema.out()), NANOARROW_OK)
      << ArrowArrayStreamGetLastError(stream.get());

  PJ::sdk::ArrowArrayHolder batch;
  ASSERT_EQ(stream.get()->get_next(stream.get(), batch.out()), NANOARROW_OK)
      << ArrowArrayStreamGetLastError(stream.get());
  ASSERT_TRUE(batch.valid());
  verifyFlatBatch(schema.get(), batch.get(), 0, 2);

  ASSERT_EQ(stream.get()->get_next(stream.get(), batch.out()), NANOARROW_OK)
      << ArrowArrayStreamGetLastError(stream.get());
  ASSERT_TRUE(batch.valid());
  verifyFlatBatch(schema.get(), batch.get(), 2, 1);
  batch.reset();
  verifyEndOfStream(stream.get());
}

/// Zstd-compressed record-batch bodies decode to the original values.
TEST(IpcDecoderTest, DecodesZstdCompressedBodies) {
  const auto bytes = test::readFile(fixturePath("flat_zstd.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto stream = std::move(*decoded);

  PJ::sdk::ArrowSchemaHolder schema;
  ASSERT_EQ(stream.get()->get_schema(stream.get(), schema.out()), NANOARROW_OK)
      << ArrowArrayStreamGetLastError(stream.get());
  PJ::sdk::ArrowArrayHolder batch;
  ASSERT_EQ(stream.get()->get_next(stream.get(), batch.out()), NANOARROW_OK)
      << ArrowArrayStreamGetLastError(stream.get());
  ASSERT_TRUE(batch.valid());
  verifyFlatBatch(schema.get(), batch.get(), 0, 3);
}

/// Lz4-compressed bodies are rejected with an actionable codec name.
TEST(IpcDecoderTest, RejectsLz4CompressedBodiesWithClearError) {
  const auto bytes = test::readFile(fixturePath("flat_lz4.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  if (!decoded) {
    EXPECT_TRUE(containsLz4(decoded.error())) << decoded.error();
    return;
  }

  auto stream = std::move(*decoded);
  PJ::sdk::ArrowArrayHolder batch;
  const int result = stream.get()->get_next(stream.get(), batch.out());
  ASSERT_NE(result, NANOARROW_OK) << "nanoarrow unexpectedly decoded an lz4-compressed body";
  EXPECT_TRUE(containsLz4(ArrowArrayStreamGetLastError(stream.get()))) << ArrowArrayStreamGetLastError(stream.get());
}

/// Empty payloads are rejected before constructing a stream.
TEST(IpcDecoderTest, RejectsEmptyInput) {
  const std::vector<uint8_t> bytes;
  const auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_FALSE(decoded);
  EXPECT_NE(decoded.error().find("parser_arrow:"), std::string::npos);
}

/// Invalid framing is surfaced at decode time without crashing.
TEST(IpcDecoderTest, RejectsGarbageInput) {
  const std::vector<uint8_t> bytes(16, uint8_t{0xFF});
  const auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_FALSE(decoded);
  EXPECT_NE(decoded.error().find("parser_arrow:"), std::string::npos);
}

/// The returned stream owns a payload copy independent of the caller's Span.
TEST(IpcDecoderTest, OwnsPayloadAfterCallerStorageIsDestroyed) {
  auto decoded = [] {
    auto temporary_bytes = test::readFile(fixturePath("flat.arrows"));
    auto result = decodeIpcStream(PJ::Span<const uint8_t>(temporary_bytes.data(), temporary_bytes.size()));
    std::fill(temporary_bytes.begin(), temporary_bytes.end(), uint8_t{0xA5});
    temporary_bytes.clear();
    temporary_bytes.shrink_to_fit();
    return result;
  }();
  ASSERT_TRUE(decoded) << decoded.error();
  auto stream = std::move(*decoded);

  PJ::sdk::ArrowSchemaHolder schema;
  ASSERT_EQ(stream.get()->get_schema(stream.get(), schema.out()), NANOARROW_OK)
      << ArrowArrayStreamGetLastError(stream.get());
  PJ::sdk::ArrowArrayHolder batch;
  ASSERT_EQ(stream.get()->get_next(stream.get(), batch.out()), NANOARROW_OK)
      << ArrowArrayStreamGetLastError(stream.get());
  ASSERT_TRUE(batch.valid());
  verifyFlatBatch(schema.get(), batch.get(), 0, 3);
}

}  // namespace
}  // namespace pj::parser_arrow
