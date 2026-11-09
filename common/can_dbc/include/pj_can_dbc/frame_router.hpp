#pragma once

// CanFrameRouter: the per-frame decode / topic / raw-fallback step shared by
// the candump importer and the live CAN streamer. One call per CAN frame:
// decode it with the bus's dictionary and append one row to
// "CAN/<bus>/<Message>"; a frame that cannot be decoded is counted and, with
// the raw fallback on, appended to "CAN/<bus>/0x<ID>" as byte0..byteN fields.

#include <array>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/signal_row.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pj_can_dbc {

/// What route() did with the frames so far. Every frame lands in exactly one of `decoded`, `unmatched`,
/// `undecodable`, `no_dictionary`.
struct RouterCounts {
  std::uint64_t decoded = 0;         ///< decoded into a message topic
  std::uint64_t unmatched = 0;       ///< the bus's dictionary has no message with that id
  std::uint64_t undecodable = 0;     ///< the id matched but the payload could not be decoded (truncated)
  std::uint64_t no_dictionary = 0;   ///< the bus has no dictionary
  std::uint64_t decoded_topics = 0;  ///< distinct message topics created by decoded frames
};

class CanFrameRouter {
 public:
  /// Largest payload (CAN FD) the raw fallback publishes: fields byte0..byte63.
  static constexpr std::size_t kMaxRawBytes = 64;

  /// `raw_fallback`: also publish frames that were not decoded as raw byte topics.
  explicit CanFrameRouter(bool raw_fallback);

  // Pinned in place: `current_` points into this object's own `buses_`, so a copy or move would leave it pointing
  // into the source. Construct it where it will live (e.g. std::optional::emplace).
  CanFrameRouter(const CanFrameRouter&) = delete;
  CanFrameRouter& operator=(const CanFrameRouter&) = delete;
  CanFrameRouter(CanFrameRouter&&) = delete;
  CanFrameRouter& operator=(CanFrameRouter&&) = delete;

  /// Dictionary for `bus`; nullptr = no dictionary on that bus. Not owned: it must outlive the router. May be
  /// called at any time, but only affects frames routed afterwards. Replacing a bus's decoder forgets its
  /// decoded-message topics (the new dictionary may name an id differently); its raw topics are kept.
  void setDecoder(std::string bus, const CanDecoder* decoder);

  /// Routes one frame. Rows go to `host` with timestamp `ts_ns` (absolute nanoseconds). A topic the host
  /// refuses to create drops that frame.
  void route(
      const PJ::sdk::SourceWriteHostView& host, std::string_view bus, std::uint32_t can_id, bool extended,
      const std::vector<std::uint8_t>& data, std::int64_t ts_ns);

  const RouterCounts& counts() const {
    return counts_;
  }

 private:
  /// Per-bus decode/topic state. Keyed internally by (id << 1) | extended rather than a per-frame string.
  struct BusState {
    const CanDecoder* decoder = nullptr;
    std::unordered_map<std::uint64_t, PJ::sdk::TopicHandle> decoded;
    std::unordered_map<std::uint64_t, PJ::sdk::TopicHandle> raw;
  };

  bool raw_fallback_;
  RouterCounts counts_;
  // node-based: a BusState pointer stays valid across later insertions.
  std::unordered_map<std::string, BusState> buses_;
  // The bus bucket is resolved on a bus-name CHANGE only: a capture typically stays on one interface for long runs.
  std::string last_bus_;
  BusState* current_ = nullptr;
  SignalRowBuilder decoded_row_builder_;
  std::array<std::string, kMaxRawBytes> byte_field_names_;
  std::vector<PJ::sdk::NamedFieldValue> raw_fields_;
};

}  // namespace pj_can_dbc
