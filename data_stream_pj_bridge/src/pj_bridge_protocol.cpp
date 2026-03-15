#include "pj_bridge_protocol.hpp"

#include <zstd.h>

#include <chrono>
#include <cstring>
#include <random>

namespace PJ::BridgeProtocol {

bool parseBinaryFrame(const uint8_t* data, size_t size,
                      std::vector<RawMessage>& messages,
                      std::vector<uint8_t>& decompress_buffer) {
  messages.clear();

  // Header: magic(4) + msg_count(4) + uncompressed_size(4) + flags(4) = 16 bytes
  if (size < 16) return false;

  uint32_t magic = 0;
  std::memcpy(&magic, data, 4);
  if (magic != kMagic) return false;

  uint32_t msg_count = 0;
  std::memcpy(&msg_count, data + 4, 4);

  // uncompressed_size at offset 8 (informational, ZSTD has its own)
  // flags at offset 12 — must be zero in current protocol version
  uint32_t flags = 0;
  std::memcpy(&flags, data + 12, 4);
  if (flags != 0) return false;

  const uint8_t* compressed = data + 16;
  size_t compressed_size = size - 16;

  // Decompress with ZSTD
  size_t decompressed_size = ZSTD_getFrameContentSize(compressed, compressed_size);
  if (decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
      decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
    return false;
  }

  constexpr size_t kMaxDecompressedSize = 256 * 1024 * 1024;  // 256 MB
  if (decompressed_size > kMaxDecompressedSize) return false;

  decompress_buffer.resize(decompressed_size);
  size_t result = ZSTD_decompress(decompress_buffer.data(), decompressed_size,
                                   compressed, compressed_size);
  if (ZSTD_isError(result)) return false;

  // Parse individual messages from decompressed payload
  const uint8_t* ptr = decompress_buffer.data();
  const uint8_t* end = ptr + result;

  for (uint32_t i = 0; i < msg_count; i++) {
    if (ptr + 2 > end) return false;
    uint16_t topic_len = 0;
    std::memcpy(&topic_len, ptr, 2);
    ptr += 2;

    if (ptr + topic_len > end) return false;
    RawMessage msg;
    msg.topic_name = std::string(reinterpret_cast<const char*>(ptr), topic_len);
    ptr += topic_len;

    if (ptr + 8 > end) return false;
    uint64_t ts = 0;
    std::memcpy(&ts, ptr, 8);
    msg.timestamp_ns = static_cast<int64_t>(ts);
    ptr += 8;

    if (ptr + 4 > end) return false;
    uint32_t cdr_len = 0;
    std::memcpy(&cdr_len, ptr, 4);
    ptr += 4;

    if (ptr + cdr_len > end) return false;
    msg.cdr_data = ptr;
    msg.cdr_size = cdr_len;
    ptr += cdr_len;

    messages.push_back(msg);
  }

  return true;
}

std::string buildRequest(const std::string& command, const std::string& uuid) {
  return R"({"op":")" + command + R"(","id":")" + uuid + R"("})";
}

std::string generateRequestId() {
  static std::mt19937 gen(static_cast<uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  static std::uniform_int_distribution<uint32_t> dist;

  char buf[37];
  auto a = dist(gen);
  auto b = dist(gen);
  auto c = dist(gen);
  auto d = dist(gen);
  snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
           a, static_cast<uint16_t>(b >> 16), static_cast<uint16_t>(b),
           static_cast<uint16_t>(c >> 16), static_cast<uint16_t>(c), d);
  return buf;
}

}  // namespace PJ::BridgeProtocol
