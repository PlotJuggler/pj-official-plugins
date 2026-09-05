#include "ipc_decoder.hpp"

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
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

/// Verify one flat fixture batch starting at the requested source row.
void verifyFlatBatch(
    const ArrowSchema* schema, const ArrowArray* batch, int64_t source_row_offset, int64_t expected_length) {
  auto array_view = test::bindArrayView(schema, batch);
  ASSERT_EQ(batch->length, expected_length);

  constexpr std::array<int64_t, 3> kTimestamps = {1000, 2000, 3000};
  constexpr std::array<double, 3> kAValues = {1.5, 2.5, 3.5};
  constexpr std::array<int64_t, 3> kBValues = {10, 20, 30};
  constexpr std::array<std::string_view, 3> kNames = {"x", "y", "z"};

  for (int64_t batch_row = 0; batch_row < batch->length; ++batch_row) {
    const auto source_row = static_cast<std::size_t>(source_row_offset + batch_row);
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(array_view->children[0], batch_row), kTimestamps[source_row]);
    EXPECT_DOUBLE_EQ(ArrowArrayViewGetDoubleUnsafe(array_view->children[1], batch_row), kAValues[source_row]);
    EXPECT_EQ(ArrowArrayViewGetIntUnsafe(array_view->children[2], batch_row), kBValues[source_row]);

    const ArrowStringView name = ArrowArrayViewGetStringUnsafe(array_view->children[3], batch_row);
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
  const auto bytes = test::readFile(test::fixturePath("flat.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto stream = std::move(*decoded);

  auto schema = test::readSchema(stream);
  ASSERT_EQ(schema.get()->n_children, 4);
  constexpr std::array<std::string_view, 4> kExpectedNames = {"timestamp_ns", "a", "b", "name"};
  constexpr std::array<std::string_view, 4> kExpectedFormats = {"l", "g", "i", "u"};
  for (std::size_t child_index = 0; child_index < kExpectedNames.size(); ++child_index) {
    EXPECT_EQ(schema.get()->children[child_index]->name, kExpectedNames[child_index]);
    EXPECT_EQ(schema.get()->children[child_index]->format, kExpectedFormats[child_index]);
  }

  auto batch = test::readBatch(stream);
  verifyFlatBatch(schema.get(), batch.get(), 0, 3);
  batch.reset();
  verifyEndOfStream(stream.get());
}

/// A stream with two record batches preserves batch boundaries and reaches EOS.
TEST(IpcDecoderTest, IteratesMultipleRecordBatches) {
  const auto bytes = test::readFile(test::fixturePath("flat_two_batches.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto stream = std::move(*decoded);

  auto schema = test::readSchema(stream);

  auto batch = test::readBatch(stream);
  verifyFlatBatch(schema.get(), batch.get(), 0, 2);

  batch = test::readBatch(stream);
  verifyFlatBatch(schema.get(), batch.get(), 2, 1);
  batch.reset();
  verifyEndOfStream(stream.get());
}

/// Zstd-compressed record-batch bodies decode to the original values.
TEST(IpcDecoderTest, DecodesZstdCompressedBodies) {
  const auto bytes = test::readFile(test::fixturePath("flat_zstd.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto stream = std::move(*decoded);

  auto schema = test::readSchema(stream);
  auto batch = test::readBatch(stream);
  verifyFlatBatch(schema.get(), batch.get(), 0, 3);
}

/// Header preflight skips compressed bodies while the reader still consumes every zstd batch.
TEST(IpcDecoderTest, DecodesMultipleZstdCompressedBodies) {
  const auto bytes = test::readFile(test::fixturePath("flat_two_batches_zstd.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto stream = std::move(*decoded);

  auto schema = test::readSchema(stream);
  auto batch = test::readBatch(stream);
  verifyFlatBatch(schema.get(), batch.get(), 0, 2);

  batch = test::readBatch(stream);
  verifyFlatBatch(schema.get(), batch.get(), 2, 1);
  batch.reset();
  verifyEndOfStream(stream.get());
}

TEST(IpcDecoderTest, DecodesLz4IncludingLaterCompressedBatches) {
  for (const char* name : {"flat_lz4.arrows", "flat_lz4_second_batch.arrows"}) {
    const auto bytes = test::readFile(test::fixturePath(name));
    auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
    ASSERT_TRUE(decoded) << decoded.error();
    auto schema = test::readSchema(*decoded);
    int64_t row = 0;
    while (true) {
      PJ::sdk::ArrowArrayHolder batch;
      ASSERT_EQ(decoded->get()->get_next(decoded->get(), batch.out()), 0);
      if (!batch.valid()) {
        break;
      }
      verifyFlatBatch(schema.get(), batch.get(), row, batch.get()->length);
      row += batch.get()->length;
    }
    EXPECT_EQ(row, 3);
  }
}

TEST(IpcDecoderTest, DecodesDictionaryValuesAfterTheRecordBoundary) {
  const auto bytes = test::readFile(test::fixturePath("dictionary.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto schema = test::readSchema(*decoded);
  auto batch = test::readBatch(*decoded);
  ASSERT_TRUE(batch.valid());
  EXPECT_EQ(schema.get()->children[1]->dictionary, nullptr);
  EXPECT_STREQ(schema.get()->children[1]->format, "u");
  auto view = test::bindArrayView(schema.get(), batch.get());
  const auto label = ArrowArrayViewGetStringUnsafe(view->children[1], 0);
  EXPECT_GT(label.size_bytes, 0);
  verifyEndOfStream(decoded->get());
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

TEST(IpcDecoderTest, DecodesViewFields) {
  const auto bytes = test::readFile(test::fixturePath("views.arrows"));
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto schema = test::readSchema(*decoded);
  auto batch = test::readBatch(*decoded);
  ASSERT_TRUE(batch.valid());
  auto view = test::bindArrayView(schema.get(), batch.get());
  EXPECT_GT(batch.get()->length, 0);
  verifyEndOfStream(decoded->get());
}

/// The borrowing decoder can consume the complete stream while its caller-owned payload remains alive and unchanged.
TEST(IpcDecoderTest, BorrowsPayloadWithoutModifyingItWhileStreamIsConsumed) {
  auto bytes = test::readFile(test::fixturePath("flat.arrows"));
  const auto original_bytes = bytes;
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  ASSERT_TRUE(decoded) << decoded.error();
  auto stream = std::move(*decoded);

  auto schema = test::readSchema(stream);
  auto batch = test::readBatch(stream);
  verifyFlatBatch(schema.get(), batch.get(), 0, 3);
  batch.reset();
  verifyEndOfStream(stream.get());
  stream.reset();
  EXPECT_EQ(bytes, original_bytes);
}

}  // namespace
}  // namespace pj::parser_arrow
