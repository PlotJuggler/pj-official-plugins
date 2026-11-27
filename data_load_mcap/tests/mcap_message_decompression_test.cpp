#include "../mcap_message_decompression.hpp"

#include <gtest/gtest.h>
#include <zstd.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace PJ::McapHelpers;

constexpr uint64_t kLargeCap = uint64_t{1} << 30;

std::vector<uint8_t> payload() {
  std::vector<uint8_t> bytes(4096);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(i % 7);
  }
  return bytes;
}

std::vector<uint8_t> compress(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out(ZSTD_compressBound(in.size()));
  const size_t written = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3);
  EXPECT_EQ(ZSTD_isError(written), 0u);
  out.resize(written);
  return out;
}

// The error text of decoding `frame`, or "" when it decodes.
std::string decodeError(const std::vector<uint8_t>& frame, uint64_t cap = kLargeCap) {
  try {
    (void)decodeZstdMessage(frame, cap);
  } catch (const std::runtime_error& e) {
    return e.what();
  }
  return {};
}

TEST(ZstdMessageDecoder, RoundTripsAFrame) {
  const auto original = payload();
  EXPECT_EQ(decodeZstdMessage(compress(original), kLargeCap), original);
}

TEST(ZstdMessageDecoder, RejectsBytesThatAreNotAFrame) {
  // A CDR payload, what an uncompressed rosbag2 message starts with.
  EXPECT_NE(decodeError({0x00, 0x01, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00}).find("not a zstd frame"), std::string::npos);
  EXPECT_NE(decodeError({}).find("not a zstd frame"), std::string::npos);
}

TEST(ZstdMessageDecoder, RejectsACorruptFrame) {
  auto frame = compress(payload());
  for (size_t i = frame.size() / 2; i < frame.size(); ++i) {
    frame[i] = static_cast<uint8_t>(frame[i] ^ 0xA5);
  }
  EXPECT_FALSE(decodeError(frame).empty());
}

TEST(ZstdMessageDecoder, RejectsTrailingBytes) {
  auto frame = compress(payload());
  frame.push_back(0x00);
  frame.push_back(0x01);
  EXPECT_NE(decodeError(frame).find("2 unexpected byte(s) after the frame"), std::string::npos) << decodeError(frame);
}

TEST(ZstdMessageDecoder, RejectsATruncatedFrame) {
  auto frame = compress(payload());
  frame.resize(frame.size() - 3);
  EXPECT_NE(decodeError(frame).find("truncated or corrupt frame"), std::string::npos) << decodeError(frame);
}

TEST(ZstdMessageDecoder, RefusesADeclaredSizeOverTheCapBeforeDecoding) {
  const auto original = payload();
  const std::string error = decodeError(compress(original), original.size() - 1);
  EXPECT_NE(error.find("declared decompressed size 4096 exceeds the limit of 4095 bytes"), std::string::npos) << error;
  EXPECT_EQ(decodeZstdMessage(compress(original), original.size()), original);
}

TEST(ZstdMessageDecoder, RejectsAFrameWithoutContentSize) {
  const auto original = payload();
  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  ASSERT_NE(cctx, nullptr);
  ASSERT_EQ(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0)), 0u);
  std::vector<uint8_t> frame(ZSTD_compressBound(original.size()));
  const size_t written = ZSTD_compress2(cctx, frame.data(), frame.size(), original.data(), original.size());
  ZSTD_freeCCtx(cctx);
  ASSERT_EQ(ZSTD_isError(written), 0u);
  frame.resize(written);

  EXPECT_NE(decodeError(frame).find("does not declare its decompressed size"), std::string::npos) << decodeError(frame);
}

TEST(MessageCodecTable, KnowsZstdOnly) {
  ASSERT_NE(findMessageCodec("zstd"), nullptr);
  EXPECT_EQ(findMessageCodec("lz4"), nullptr);
  EXPECT_EQ(findMessageCodec(""), nullptr);
  EXPECT_EQ(findMessageCodec("ZSTD"), nullptr);
}

TEST(MessageCodecTable, ErrorsNameTheFormat) {
  const MessageCodec* codec = findMessageCodec("zstd");
  ASSERT_NE(codec, nullptr);
  const std::vector<uint8_t> not_a_frame{0x00, 0x01, 0x00, 0x00};
  try {
    (void)decodeMessagePayload(*codec, not_a_frame, kLargeCap);
    FAIL() << "expected a decode failure";
  } catch (const std::runtime_error& e) {
    EXPECT_EQ(std::string(e.what()), "per-message zstd decompression failed: not a zstd frame");
  }
}

TEST(PayloadCompression, OnlyMessageModeDecodes) {
  const std::optional<std::string> zstd = "zstd";
  EXPECT_EQ(resolvePayloadCompression(std::nullopt, zstd).codec, nullptr);
  for (const auto* mode : {"", "none", "NONE", "file", "FILE"}) {
    const auto resolved = resolvePayloadCompression(std::string(mode), zstd);
    EXPECT_EQ(resolved.codec, nullptr) << mode;
    EXPECT_FALSE(resolved.unsupported_format.has_value()) << mode;
  }
  // rosbag2 writes the mode upper case; hand-written metadata usually lower case.
  for (const auto* mode : {"MESSAGE", "message", "Message"}) {
    const auto resolved = resolvePayloadCompression(std::string(mode), zstd);
    ASSERT_NE(resolved.codec, nullptr) << mode;
    EXPECT_EQ(resolved.codec->format, "zstd");
    EXPECT_FALSE(resolved.unsupported_format.has_value()) << mode;
  }
}

TEST(PayloadCompression, MessageModeWithAnUnknownOrMissingFormatIsUnsupported) {
  const auto lz4 = resolvePayloadCompression(std::string("MESSAGE"), std::string("lz4"));
  EXPECT_EQ(lz4.codec, nullptr);
  EXPECT_EQ(lz4.unsupported_format, "lz4");

  const auto missing = resolvePayloadCompression(std::string("MESSAGE"), std::nullopt);
  EXPECT_EQ(missing.codec, nullptr);
  EXPECT_EQ(missing.unsupported_format, "");
}

}  // namespace
