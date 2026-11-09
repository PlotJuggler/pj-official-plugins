#include <algorithm>
#include <pj_can_dbc/can_topic.hpp>
#include <pj_can_dbc/frame_router.hpp>
#include <utility>

namespace pj_can_dbc {

namespace {

std::uint64_t frameKey(std::uint32_t can_id, bool extended) {
  return (static_cast<std::uint64_t>(can_id) << 1) | (extended ? 1u : 0u);
}

}  // namespace

CanFrameRouter::CanFrameRouter(bool raw_fallback) : raw_fallback_(raw_fallback) {
  for (std::size_t i = 0; i < byte_field_names_.size(); ++i) {
    byte_field_names_[i] = "byte" + std::to_string(i);
  }
}

void CanFrameRouter::setDecoder(std::string bus, const CanDecoder* decoder) {
  BusState& state = buses_[std::move(bus)];
  state.decoder = decoder;
  state.decoded.clear();
}

void CanFrameRouter::route(
    const PJ::sdk::SourceWriteHostView& host, std::string_view bus, std::uint32_t can_id, bool extended,
    const std::vector<std::uint8_t>& data, std::int64_t ts_ns) {
  if (current_ == nullptr || bus != last_bus_) {
    last_bus_.assign(bus);
    current_ = &buses_[last_bus_];
  }
  BusState& state = *current_;
  // Built once per frame, reused by whichever of decoded/raw applies.
  const std::uint64_t key = frameKey(can_id, extended);

  DecodeResult result = DecodeResult::kNoMatch;
  std::vector<DecodedSignal> signals;
  if (state.decoder != nullptr) {
    signals = state.decoder->decode(can_id, extended, data, result);
  }

  if (result == DecodeResult::kDecoded && !signals.empty()) {
    ++counts_.decoded;
    auto it = state.decoded.find(key);
    if (it == state.decoded.end()) {
      auto topic = host.ensureTopic(canTopicName(bus, state.decoder->messageName(can_id, extended), can_id));
      if (!topic) {
        return;  // best-effort: this frame is dropped
      }
      it = state.decoded.emplace(key, *topic).first;
      ++counts_.decoded_topics;
    }
    (void)host.appendRecord(it->second, PJ::Timestamp{ts_ns}, decoded_row_builder_.build(signals));
    return;
  }

  if (state.decoder == nullptr) {
    ++counts_.no_dictionary;
  } else if (result == DecodeResult::kUndecodable) {
    ++counts_.undecodable;
  } else {
    ++counts_.unmatched;
  }

  if (!raw_fallback_) {
    return;
  }
  auto it = state.raw.find(key);
  if (it == state.raw.end()) {
    auto topic = host.ensureTopic(canTopicName(bus, std::string(), can_id));
    if (!topic) {
      return;
    }
    it = state.raw.emplace(key, *topic).first;
  }
  const std::size_t count = std::min(data.size(), kMaxRawBytes);
  raw_fields_.clear();
  raw_fields_.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    raw_fields_.push_back({.name = byte_field_names_[i], .value = static_cast<std::int64_t>(data[i])});
  }
  (void)host.appendRecord(
      it->second, PJ::Timestamp{ts_ns},
      PJ::Span<const PJ::sdk::NamedFieldValue>(raw_fields_.data(), raw_fields_.size()));
}

}  // namespace pj_can_dbc
