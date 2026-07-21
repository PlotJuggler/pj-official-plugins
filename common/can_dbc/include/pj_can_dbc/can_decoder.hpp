#pragma once

// CanDecoder: decodes raw CAN frames into named physical signals using one or
// more DBC databases. The DBC library (dbc_parser_cpp) is fully hidden behind a
// pImpl so no other translation unit depends on it — this header pulls in NO
// dbc_parser_cpp / fast_float headers.

#include <cstdint>
#include <memory>
#include <pj_base/expected.hpp>
#include <string>
#include <vector>

namespace mf4_detail {

/// One decoded signal from a CAN frame (physical/engineering value).
struct DecodedSignal {
  std::string name;
  double value = 0.0;
  std::string unit;
};

/// Outcome of decoding one frame. kUndecodable means the id matched a message
/// but the payload could not be decoded (truncated frame, CAN FD payload
/// > 8 bytes, ...) — callers should count these; they are not "unmatched".
enum class DecodeResult {
  kNoMatch,
  kDecoded,
  kUndecodable,
};

class CanDecoder {
 public:
  CanDecoder();
  ~CanDecoder();
  CanDecoder(const CanDecoder&) = delete;
  CanDecoder& operator=(const CanDecoder&) = delete;

  /// Load and accumulate a DBC database from a file. Multiple calls merge.
  PJ::Status loadDbcFile(const std::string& path);
  /// Load and accumulate a DBC database from an in-memory string.
  PJ::Status loadDbcString(const std::string& dbc_text);

  /// Number of message definitions currently loaded.
  std::size_t messageCount() const;

  /// Name of the message matching `can_id`/`extended` (same rules as decode()),
  /// or an empty string if none matches. Used to name the output topic.
  std::string messageName(std::uint32_t can_id, bool extended) const;

  /// Decode one CAN frame. `can_id` is the raw 11/29-bit identifier
  /// (mdf CanMessage::CanId()); `extended` is its frame format
  /// (CanMessage::ExtendedId()). An extended frame first tries the id with the
  /// DBC extended-frame flag (0x80000000, Vector convention) set; if absent it
  /// falls back to the raw id, but only when the id is above the 11-bit range
  /// (J1939-style DBCs store 29-bit ids raw) — a low id never silently decodes
  /// an extended frame with a standard message's layout. `result` reports
  /// match/decoded/undecodable; the returned signals are empty unless kDecoded.
  ///
  /// Known dbc_parser_cpp limitations: multiplexed signals are silently skipped
  /// (only non-multiplexed signals in a message decode); unsigned 64-bit raw
  /// values above 2^53 lose precision (double). A standard and an extended
  /// message that share a numeric id in an ambiguous DBC (extended flag not
  /// set) cannot be told apart.
  std::vector<DecodedSignal> decode(
      std::uint32_t can_id, bool extended, const std::vector<std::uint8_t>& data, DecodeResult& result) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mf4_detail
