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

  /// Decode one CAN frame. `can_id` is the raw 11/29-bit id (mdf CanMessage::CanId());
  /// matching ignores the extended-id flag bit. Sets `matched` to true iff a
  /// message with this id exists in a loaded DBC. Returns the decoded signals
  /// (empty if unmatched, or if the matched message failed to decode).
  std::vector<DecodedSignal> decode(std::uint32_t can_id, const std::vector<std::uint8_t>& data, bool& matched) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mf4_detail
