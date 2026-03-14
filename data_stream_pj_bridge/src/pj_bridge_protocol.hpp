#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace PJ::BridgeProtocol {

/// Magic bytes at the start of every binary data frame: "PJRB" (PlotJuggler Raw Bridge).
constexpr uint32_t kMagic = 0x42524A50;  // "PJRB" in little-endian

struct TopicInfo {
  std::string name;
  std::string encoding;
  std::string schema_name;
  std::string schema;
};

struct RawMessage {
  std::string topic_name;
  int64_t timestamp_ns = 0;
  /// Points into decompress_buffer passed to parseBinaryFrame().
  /// Valid only until the next call to parseBinaryFrame() or buffer modification.
  const uint8_t* cdr_data = nullptr;
  size_t cdr_size = 0;
};

/// Parse a binary data frame into individual raw messages.
/// Frame format: [magic:4][msg_count:4][zstd_compressed_payload]
/// Each message in payload: [topic_len:4][topic:N][timestamp_ns:8][cdr_len:4][cdr:M]
bool parseBinaryFrame(const uint8_t* data, size_t size,
                      std::vector<RawMessage>& messages,
                      std::vector<uint8_t>& decompress_buffer);

/// Build a JSON request with a UUID.
std::string buildRequest(const std::string& command, const std::string& uuid);

/// Generate a simple UUID-like string for request matching.
std::string generateRequestId();

}  // namespace PJ::BridgeProtocol
