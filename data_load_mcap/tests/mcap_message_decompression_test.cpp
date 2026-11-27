#include "../mcap_message_decompression.hpp"

#include <gtest/gtest.h>
#include <zstd.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace PJ::McapHelpers;

constexpr uint64_t kLargeCap = uint64_t{1} << 30;
const std::vector<uint8_t> kCdrInt32{0x00, 0x01, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00};

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

std::vector<uint8_t> decode(const std::vector<uint8_t>& frame, uint64_t cap = kLargeCap) {
  const PJ::sdk::PayloadView view = decodeZstdMessage({frame.data(), frame.size()}, cap);
  return {view.bytes.data(), view.bytes.data() + view.bytes.size()};
}

// The error text of decoding `frame`, or "" when it decodes.
std::string decodeError(const std::vector<uint8_t>& frame, uint64_t cap = kLargeCap) {
  try {
    (void)decode(frame, cap);
  } catch (const std::runtime_error& e) {
    return e.what();
  }
  return {};
}

TEST(ZstdMessageDecoder, RecognisesFramesByMagic) {
  const auto frame = compress(payload());
  EXPECT_TRUE(isZstdFrame(frame.data(), frame.size()));
  EXPECT_FALSE(isZstdFrame(kCdrInt32.data(), kCdrInt32.size()));
  const std::vector<uint8_t> magic_only{0x28, 0xB5, 0x2F};
  EXPECT_FALSE(isZstdFrame(magic_only.data(), magic_only.size()));
  EXPECT_FALSE(isZstdFrame(nullptr, 0));
}

TEST(ZstdMessageDecoder, RoundTripsAFrame) {
  const auto original = payload();
  EXPECT_EQ(decode(compress(original)), original);
  EXPECT_EQ(decode(compress(kCdrInt32)), kCdrInt32);
}

TEST(ZstdMessageDecoder, ErrorsNameTheCodec) {
  EXPECT_EQ(decodeError(kCdrInt32), "zstd decompression failed: not a zstd frame");
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
  EXPECT_EQ(decode(compress(original), original.size()), original);
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

}  // namespace
