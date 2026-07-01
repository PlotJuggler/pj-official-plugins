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

  /// Decode one CAN frame. `can_id` is the raw 11/29-bit identifier
  /// (mdf CanMessage::CanId()); `extended` is its frame format
  /// (CanMessage::ExtendedId()). Matching tries the raw id first, then — for
  /// extended frames — the id with the DBC extended-frame flag (0x80000000) set,
  /// so databases following the Vector convention (bit 31 set on extended
  /// messages) match without confusing a standard and an extended message that
  /// share a numeric id. Sets `matched` to true iff a message with this id
  /// exists. Returns the decoded signals (empty if unmatched, if the frame is
  /// shorter than the message, or if decoding fails).
  ///
  /// Known dbc_parser_cpp limitations: multiplexed signals are silently skipped
  /// (only non-multiplexed signals in a message decode); signals wider than
  /// 32 bits may be imprecise. A standard and an extended message that share a
  /// numeric id in an ambiguous DBC (extended flag not set) cannot be told apart.
  std::vector<DecodedSignal> decode(
      std::uint32_t can_id, bool extended, const std::vector<std::uint8_t>& data, bool& matched) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mf4_detail
