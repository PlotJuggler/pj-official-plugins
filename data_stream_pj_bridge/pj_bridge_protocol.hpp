#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
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

  /// Value equality over all fields — lets the advertise catalog detect a real
  /// change (schema/type edit, not just a re-notification) so the source
  /// re-advertises to the host only on a genuine delta. See applyAdvertiseDelta.
  friend bool operator==(const TopicInfo&, const TopicInfo&) = default;
};

struct RawMessage {
  std::string topic_name;
  int64_t timestamp_ns = 0;
  /// Points into decompress_buffer passed to parseBinaryFrame().
  /// Valid only until the next call to parseBinaryFrame() or buffer modification.
  const uint8_t* cdr_data = nullptr;
  size_t cdr_size = 0;
};

/// Largest decompressed payload a data frame may declare; bigger frames are rejected.
inline constexpr size_t kMaxDecompressedSize = size_t{256} * 1024 * 1024;

/// The decompressed size a frame declares (its ZSTD frame content size), or nullopt when it is
/// unknown, an error sentinel, or above kMaxDecompressedSize. Takes the raw 64-bit value: on
/// 32-bit targets (wasm32) narrowing to size_t first would let a sentinel through as a small size.
[[nodiscard]] std::optional<size_t> checkedDecompressedSize(uint64_t content_size);

/// Parse a binary data frame into individual raw messages.
/// Frame format: [magic:4][msg_count:4][zstd_compressed_payload]
/// Each message in payload: [topic_len:2][topic:N][timestamp_ns:8][cdr_len:4][cdr:M]
bool parseBinaryFrame(
    const uint8_t* data, size_t size, std::vector<RawMessage>& messages, std::vector<uint8_t>& decompress_buffer);

/// Build a JSON request with a UUID.
std::string buildRequest(const std::string& command, const std::string& uuid);

/// Generate a simple UUID-like string for request matching.
std::string generateRequestId();

}  // namespace PJ::BridgeProtocol
