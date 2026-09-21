#include <algorithm>
#include <cmath>
#include <exception>
#include <libdbc/dbc.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace pj_can_dbc {

namespace {

/// Normalizes a raw `VAL_` key (`Signal::ValueDescription::value`, a
/// `uint32_t` bit pattern -- see the vendored dbc.cpp patch in
/// common/can_dbc/CMakeLists.txt) to the signed integer the signal actually
/// decodes to. Unsigned signals take the key as-is. Signed signals first
/// reinterpret it as a two's-complement 32-bit value (the convention our
/// vendored patch produces for a literal negative key, e.g. "-1"), then --
/// for a signal narrower than 32 bits -- fold the equivalent ALL-UNSIGNED
/// convention some DBC exporters use instead (e.g. "255" for an 8-bit
/// signal's -1) onto the same negative key, so both spellings of the same
/// fault value land on one table entry.
std::int64_t normalizeValueKey(std::uint32_t raw_key, bool is_signed, std::uint32_t size) {
  if (!is_signed) {
    return static_cast<std::int64_t>(raw_key);
  }
  std::int64_t key = static_cast<std::int64_t>(static_cast<std::int32_t>(raw_key));
  if (size < 32) {
    const std::int64_t width = std::int64_t{1} << size;
    const std::int64_t half = std::int64_t{1} << (size - 1);
    if (key >= half && key < width) {
      key -= width;
    }
  }
  return key;
}

/// DBC extended-frame flag (Vector convention: bit 31 set on 29-bit messages).
constexpr std::uint32_t kExtendedFlag = 0x8000'0000u;
/// Highest standard (11-bit) CAN id.
constexpr std::uint32_t kMaxStandardId = 0x7FFu;
/// CAN payload is at most 8 bytes; a signal must fit inside 64 bits.
constexpr std::uint32_t kMaxSignalBits = 64;

/// A signal the pinned dbc_parser_cpp can decode without an out-of-range shift:
/// size in [1, 64] and start bit inside the 64-bit payload. A malformed DBC
/// with a wider signal would shift a 64-bit value by >= 64 (undefined).
bool signalLayoutInRange(const Libdbc::Signal& sig) {
  return sig.size >= 1 && sig.size <= kMaxSignalBits && sig.start_bit < kMaxSignalBits;
}

bool messageLayoutInRange(const Libdbc::Message& msg) {
  for (const auto& sig : msg.get_signals()) {
    if (!signalLayoutInRange(sig)) {
      return false;
    }
  }
  return true;
}
}  // namespace

/// Name/unit/value-table of one signal, cached at DBC load.
/// Libdbc::Message::get_signals() returns the whole Signal vector *by value*
/// (each Signal owns strings plus unused receiver/value-description
/// vectors), so calling it per frame would deep-copy on every decode;
/// decode() only needs what is cached here.
///
/// `labels` holds this signal's `VAL_` table (empty when it has none, or
/// when `factor == 0` -- the raw-value round trip below is meaningless
/// then), sorted by key and deduped keeping the FIRST entry for a
/// duplicate key (a malformed DBC repeating a key is not our problem to
/// referee). The label strings are owned here so `DecodedSignal::label`
/// (a view) stays valid for the decoder's lifetime. `label_name` is
/// `"<name>_label"`, computed once here (iff `labels` is non-empty) instead
/// of per decoded frame; `DecodedSignal::label_name` views it the same way.
struct SignalMeta {
  std::string name;
  std::string unit;
  double factor = 1.0;
  double offset = 0.0;
  std::vector<std::pair<std::int64_t, std::string>> labels;
  std::string label_name;
};

/// Builds `SignalMeta::labels` from a signal's raw `VAL_` table: normalizes
/// each key (see normalizeValueKey), sorts by key, and drops a duplicate
/// key's later entry.
std::vector<std::pair<std::int64_t, std::string>> buildLabels(const Libdbc::Signal& sig) {
  std::vector<std::pair<std::int64_t, std::string>> labels;
  if (sig.factor == 0.0 || sig.value_descriptions.empty()) {
    return labels;
  }
  labels.reserve(sig.value_descriptions.size());
  for (const auto& desc : sig.value_descriptions) {
    labels.emplace_back(normalizeValueKey(desc.value, sig.is_signed, sig.size), desc.description);
  }
  std::stable_sort(labels.begin(), labels.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  labels.erase(
      std::unique(labels.begin(), labels.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
      labels.end());
  return labels;
}

struct MessageEntry {
  Libdbc::Message message;
  std::vector<SignalMeta> signals;  // parallel to message.parse_signals() output
};

struct CanDecoder::Impl {
  std::vector<MessageEntry> messages;
  // Keyed by the DBC message id exactly as stored (raw, possibly with the
  // extended flag). Lookup handles both the raw and Vector-flagged forms.
  std::unordered_map<std::uint32_t, std::size_t> id_to_index;

  void addFrom(const Libdbc::DbcParser& parser) {
    for (const auto& msg : parser.get_messages()) {
      // Drop messages whose signal layout would make dbc_parser_cpp shift a
      // 64-bit value out of range (undefined behavior) on decode.
      if (!messageLayoutInRange(msg)) {
        continue;
      }
      std::vector<SignalMeta> meta;
      for (const auto& sig : msg.get_signals()) {
        SignalMeta signal_meta{sig.name, sig.unit, sig.factor, sig.offset, buildLabels(sig), {}};
        if (!signal_meta.labels.empty()) {
          signal_meta.label_name = signal_meta.name + "_label";
        }
        meta.push_back(std::move(signal_meta));
      }
      id_to_index[msg.id()] = messages.size();
      messages.push_back(MessageEntry{msg, std::move(meta)});
    }
  }

  const MessageEntry* find(std::uint32_t can_id, bool extended) const {
    const auto resolve = [this](std::unordered_map<std::uint32_t, std::size_t>::const_iterator it) {
      return it == id_to_index.end() ? nullptr : &messages[it->second];
    };
    if (extended) {
      // Prefer the Vector-flagged 29-bit message so an extended frame is not
      // shadowed by a standard message sharing the same numeric id. Fall back
      // to the raw id for DBCs that store 29-bit ids without the flag
      // (J1939-style), but only above the 11-bit range — a raw entry <= 0x7FF
      // is a standard message and must not decode an extended frame.
      auto it = id_to_index.find(can_id | kExtendedFlag);
      if (it == id_to_index.end() && can_id > kMaxStandardId) {
        it = id_to_index.find(can_id);
      }
      return resolve(it);
    }
    return resolve(id_to_index.find(can_id));
  }
};

CanDecoder::CanDecoder() : impl_(std::make_unique<Impl>()) {}
CanDecoder::~CanDecoder() = default;

PJ::Status CanDecoder::loadDbcString(const std::string& dbc_text) {
  Libdbc::DbcParser parser;
  std::istringstream stream(dbc_text);
  try {
    parser.parse_file(stream);
  } catch (const std::exception& err) {
    return PJ::unexpected(std::string("can_dbc: DBC parse error: ") + err.what());
  }
  impl_->addFrom(parser);
  return PJ::okStatus();
}

PJ::Status CanDecoder::loadDbcFile(const std::string& path) {
  Libdbc::DbcParser parser;
  try {
    parser.parse_file(path);
  } catch (const std::exception& err) {
    return PJ::unexpected(std::string("can_dbc: DBC parse error (") + path + "): " + err.what());
  }
  impl_->addFrom(parser);
  return PJ::okStatus();
}

std::size_t CanDecoder::messageCount() const {
  return impl_->messages.size();
}

std::size_t CanDecoder::valueTableCount() const {
  std::size_t count = 0;
  for (const auto& entry : impl_->messages) {
    for (const auto& sig : entry.signals) {
      count += sig.labels.empty() ? 0 : 1;
    }
  }
  return count;
}

std::string CanDecoder::messageName(std::uint32_t can_id, bool extended) const {
  const MessageEntry* entry = impl_->find(can_id, extended);
  return entry != nullptr ? entry->message.name() : std::string{};
}

std::vector<DecodedSignal> CanDecoder::decode(
    std::uint32_t can_id, bool extended, const std::vector<std::uint8_t>& data, DecodeResult& result) const {
  result = DecodeResult::kNoMatch;
  std::vector<DecodedSignal> out;

  const MessageEntry* entry = impl_->find(can_id, extended);
  if (entry == nullptr) {
    return out;
  }
  result = DecodeResult::kUndecodable;
  const Libdbc::Message& msg = entry->message;

  // Reject frames shorter than the message: dbc_parser_cpp would zero-fill the
  // missing bytes and return "success" with silently wrong signal values.
  if (data.size() < static_cast<std::size_t>(msg.size())) {
    return out;  // matched, but the frame is truncated -> cannot decode
  }

  std::vector<double> values;
  if (msg.parse_signals(data, values) != Libdbc::Message::ParseSignalsStatus::Success) {
    return out;  // matched but undecodable (e.g. CAN FD payload > 8 bytes)
  }
  result = DecodeResult::kDecoded;

  const auto& signals = entry->signals;  // cached name/unit/labels, no per-frame copy
  const std::size_t count = signals.size() < values.size() ? signals.size() : values.size();
  out.reserve(count);
  for (std::size_t k = 0; k < count; ++k) {
    const SignalMeta& meta = signals[k];
    DecodedSignal decoded{.name = meta.name, .value = values[k], .unit = meta.unit};
    if (!meta.labels.empty()) {
      decoded.label_name = meta.label_name;
      // Recover the raw integer value from the physical value (inverse of
      // dbc_parser_cpp's `value = raw * factor + offset`); factor == 0 was
      // already excluded from having any labels at load time (buildLabels).
      const std::int64_t raw = std::llround((values[k] - meta.offset) / meta.factor);
      decoded.raw = raw;
      const auto it = std::lower_bound(
          meta.labels.begin(), meta.labels.end(), raw,
          [](const auto& label_entry, std::int64_t key) { return label_entry.first < key; });
      if (it != meta.labels.end() && it->first == raw) {
        decoded.label = std::string_view(it->second);
      }
    }
    out.push_back(std::move(decoded));
  }
  return out;
}

}  // namespace pj_can_dbc
