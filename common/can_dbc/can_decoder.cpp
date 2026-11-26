#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <libdbc/dbc.hpp>
#include <libdbc/exceptions/error.hpp>
#include <optional>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/dictionary_paths.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pj_can_dbc {

namespace {

/// Maps a raw `VAL_` key (`Signal::ValueDescription::value`, a `uint32_t` bit pattern, see
/// cmake/dbc_parser.cmake) to the signed integer the signal decodes to. A signed key is read as 32-bit
/// two's complement (what the patch produces for a literal "-1"); for a signal narrower than 32 bits the
/// all-unsigned spelling some exporters use ("255" for an 8-bit -1) is folded onto the same negative key,
/// so both spellings share one table entry.
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
constexpr std::uint32_t kMaxStandardId = 0x7FFu;
constexpr std::uint32_t kMaxExtendedId = 0x1FFF'FFFFu;
/// A signal's value must fit in 64 bits.
constexpr std::uint32_t kMaxSignalBits = 64;
/// Frames up to this long decode through dbc_parser_cpp; longer ones (CAN FD, payloads reassembled from several
/// frames) go through extractRawBits(), because the parser refuses them.
constexpr std::size_t kClassicPayloadBytes = 8;

/// A signal both decode paths read without an out-of-range shift: size in [1, 64] and start bit below 64 for a
/// message of up to 8 bytes (the parser shifts a 64-bit value by the start bit, UB at >= 64), below the declared
/// length for a longer one (only frames at least that long reach decode(), so it never meets the parser).
bool signalLayoutInRange(const Libdbc::Signal& sig, std::uint32_t message_bits) {
  return sig.size >= 1 && sig.size <= kMaxSignalBits && sig.start_bit < std::max(kMaxSignalBits, message_bits);
}

bool messageLayoutInRange(const Libdbc::Message& msg) {
  const std::uint32_t message_bits = std::uint32_t{msg.size()} * 8u;
  for (const auto& sig : msg.get_signals()) {
    if (!signalLayoutInRange(sig, message_bits)) {
      return false;
    }
  }
  return true;
}
}  // namespace

/// Name, unit, value table and bit layout of one signal, cached at DBC load: Libdbc::Message::get_signals()
/// returns the Signal vector by value, so calling it per frame would deep-copy on every decode.
///
/// `labels` is the signal's `VAL_` table, sorted by key, first entry kept for a duplicate key; empty when the
/// signal has none or `factor == 0` (the raw-value round trip is meaningless then). The strings are owned here so
/// `DecodedSignal::label` (a view) stays valid for the decoder's lifetime. `label_name` names the text field
/// carrying the label, allocated once at load iff `labels` is non-empty (see assignLabelNames);
/// `DecodedSignal::label_name` views it.
struct SignalMeta {
  std::string name;
  std::string unit;
  double factor = 1.0;
  double offset = 0.0;
  std::vector<std::pair<std::int64_t, std::string>> labels;
  std::string label_name;
  // Bit layout, read by extractRawBits()/physicalValue() for payloads longer than 8 bytes.
  std::uint32_t start_bit = 0;
  std::uint32_t size = 0;
  bool is_bigendian = false;
  bool is_signed = false;
};

namespace {

/// Raw bits of one signal in a payload longer than 8 bytes, numbered as dbc_parser_cpp numbers a classic frame:
/// Intel (`@1`) counts up from bit 0 = the LSB of byte 0; Motorola (`@0`) names the signal's MSB and runs in
/// big-endian ("sawtooth") order, the parser's python-can conversion extended past byte 7. nullopt when the
/// signal reaches past the payload.
std::optional<std::uint64_t> extractRawBits(const std::vector<std::uint8_t>& data, const SignalMeta& sig) {
  const std::size_t payload_bits = data.size() * 8;
  const auto bitAt = [&data](std::size_t byte, std::size_t bit) -> std::uint64_t {
    return (static_cast<std::uint64_t>(data[byte]) >> bit) & 1u;
  };
  std::uint64_t value = 0;
  if (sig.is_bigendian) {
    // Position of the MSB counting from the MSB of byte 0; the signal runs on from there.
    const std::size_t first = 8 * (sig.start_bit / 8) + (7 - sig.start_bit % 8);
    if (first + sig.size > payload_bits) {
      return std::nullopt;
    }
    for (std::size_t pos = first; pos < first + sig.size; ++pos) {
      value = (value << 1) | bitAt(pos / 8, 7 - pos % 8);
    }
  } else {
    if (std::size_t{sig.start_bit} + sig.size > payload_bits) {
      return std::nullopt;
    }
    for (std::size_t pos = std::size_t{sig.start_bit} + sig.size; pos-- > sig.start_bit;) {
      value = (value << 1) | bitAt(pos / 8, pos % 8);
    }
  }
  return value;
}

/// Two's complement for a signed signal wider than 1 bit (a 1-bit signed signal stays 0/1, as in
/// dbc_parser_cpp), then `raw * factor + offset`.
double physicalValue(std::uint64_t raw, const SignalMeta& sig) {
  if (!sig.is_signed || sig.size < 2) {
    return static_cast<double>(raw) * sig.factor + sig.offset;
  }
  std::int64_t value = static_cast<std::int64_t>(raw);
  if (sig.size < 64) {
    const std::int64_t sign_bit = std::int64_t{1} << (sig.size - 1);
    value = (value ^ sign_bit) - sign_bit;
  }
  return static_cast<double>(value) * sig.factor + sig.offset;
}

}  // namespace

/// The signal's `VAL_` table with normalized keys (see normalizeValueKey), sorted; the first entry of a
/// duplicate key wins.
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

/// Names each `VAL_` signal's text field `"<name>_label"`, or `"<name>_label_2"`, `"_3"`, ... when that name is
/// taken in the message by a real signal (a DBC may pair `Foo` with a genuine `Foo_label`) or by an earlier
/// label, so a row never carries two fields with one name. Allocation walks signals in declaration order
/// against all the message's real signal names, so it depends only on the DBC. Collisions are per message
/// (each message is its own topic).
void assignLabelNames(std::vector<SignalMeta>& meta) {
  if (std::none_of(meta.begin(), meta.end(), [](const SignalMeta& m) { return !m.labels.empty(); })) {
    return;  // the common case: nothing to name, so skip building the set
  }
  std::unordered_set<std::string> taken;
  taken.reserve(meta.size() * 2);
  for (const auto& signal_meta : meta) {
    taken.insert(signal_meta.name);
  }
  for (auto& signal_meta : meta) {
    if (signal_meta.labels.empty()) {
      continue;
    }
    const std::string base = signal_meta.name + "_label";
    std::string candidate = base;
    for (std::size_t n = 2; taken.count(candidate) != 0; ++n) {
      candidate = base + "_" + std::to_string(n);
    }
    taken.insert(candidate);
    signal_meta.label_name = std::move(candidate);
  }
}

struct MessageEntry {
  Libdbc::Message message;
  std::vector<SignalMeta> signals;  // parallel to message.parse_signals() output
};

struct CanDecoder::Impl {
  std::vector<MessageEntry> messages;
  // Keyed by the DBC message id as stored (possibly with the extended flag); find() handles both forms.
  std::unordered_map<std::uint32_t, std::size_t> id_to_index;
  // The 29-bit messages for CanProtocol::kJ1939 / kNmea2000, first loaded wins in both: by id
  // without priority (the message written for one sender) and by PGN (any sender).
  std::unordered_map<std::uint32_t, std::size_t> sender_to_index;
  std::unordered_map<std::uint32_t, std::size_t> pgn_to_index;
  CanProtocol protocol = CanProtocol::kRaw;

  void addFrom(const Libdbc::DbcParser& parser) {
    for (const auto& msg : parser.get_messages()) {
      // Drop messages with a signal neither decode path can read (see signalLayoutInRange()).
      if (!messageLayoutInRange(msg)) {
        continue;
      }
      std::vector<SignalMeta> meta;
      for (const auto& sig : msg.get_signals()) {
        meta.push_back(
            SignalMeta{
                .name = sig.name,
                .unit = sig.unit,
                .factor = sig.factor,
                .offset = sig.offset,
                .labels = buildLabels(sig),
                .label_name = {},
                .start_bit = sig.start_bit,
                .size = sig.size,
                .is_bigendian = sig.is_bigendian,
                .is_signed = sig.is_signed});
      }
      assignLabelNames(meta);
      // 29-bit: the Vector flag set, or a raw id above the 11-bit range (as find() reads it).
      // An id that does not fit in 29 bits is no CAN id and would claim PGN 0: Vector's
      // VECTOR__INDEPENDENT_SIG_MSG placeholder for orphan signals is 0xC0000000.
      const std::uint32_t id29 = msg.id() & ~kExtendedFlag;
      if (((msg.id() & kExtendedFlag) != 0 || msg.id() > kMaxStandardId) && id29 <= kMaxExtendedId) {
        sender_to_index.emplace(withoutPriority(id29), messages.size());
        pgn_to_index.emplace(pgnFromCanId(id29), messages.size());
      }
      id_to_index[msg.id()] = messages.size();
      messages.push_back(MessageEntry{msg, std::move(meta)});
    }
  }

  const MessageEntry* find(std::uint32_t can_id, bool extended) const {
    const auto resolve = [this](std::unordered_map<std::uint32_t, std::size_t>::const_iterator it) {
      return it == id_to_index.end() ? nullptr : &messages[it->second];
    };
    if (extended && protocol != CanProtocol::kRaw) {
      // The message written for this very sender wins over the PGN's first one: an OEM DBC
      // may define one PGN once per source, each with its own layout.
      if (const auto sender = sender_to_index.find(withoutPriority(can_id)); sender != sender_to_index.end()) {
        return &messages[sender->second];
      }
      const auto it = pgn_to_index.find(pgnFromCanId(can_id));
      return it == pgn_to_index.end() ? nullptr : &messages[it->second];
    }
    if (extended) {
      // Prefer the Vector-flagged 29-bit message so a standard message with the same numeric id does not shadow
      // it. Fall back to the raw id for DBCs that store 29-bit ids without the flag (J1939-style), but only above
      // the 11-bit range: a raw entry <= 0x7FF is a standard message and must not decode an extended frame.
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
  // Libdbc's parse_file(path) accepts only a case-sensitive ".dbc" suffix; keep that gate and its message, but
  // read the file ourselves.
  if (const auto dot = path.find_last_of('.');
      dot == std::string::npos || path.compare(dot, std::string::npos, ".dbc") != 0) {
    const std::string extension = dot == std::string::npos ? std::string() : path.substr(dot);
    return PJ::unexpected(
        std::string("can_dbc: DBC parse error (") + path +
        "): " + Libdbc::NonDbcFileFormatError(path, extension).what());
  }
  auto text = readAuxFile(path, "DBC");
  if (!text) {
    return PJ::unexpected(text.error());
  }
  Libdbc::DbcParser parser;
  std::istringstream stream(*text);
  try {
    parser.parse_file(stream);
  } catch (const std::exception& err) {
    return PJ::unexpected(std::string("can_dbc: DBC parse error (") + path + "): " + err.what());
  }
  impl_->addFrom(parser);
  return PJ::okStatus();
}

void CanDecoder::setProtocol(CanProtocol protocol) {
  impl_->protocol = protocol;
}

CanProtocol CanDecoder::protocol() const {
  return impl_->protocol;
}

bool CanDecoder::isFastPacket(std::uint32_t can_id, bool extended) const {
  if (impl_->protocol != CanProtocol::kNmea2000 || !extended) {
    return false;
  }
  const MessageEntry* entry = impl_->find(can_id, extended);
  return entry != nullptr && entry->message.size() > kClassicPayloadBytes;
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
    std::uint32_t can_id, bool extended, const std::vector<std::uint8_t>& data, DecodeResult& result,
    bool variable_length) const {
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
  if (!variable_length && data.size() < static_cast<std::size_t>(msg.size())) {
    return out;
  }

  const auto& signals = entry->signals;
  std::vector<double> values;
  // Variable length only: false for a signal past the payload's end, which is left out of the result.
  std::vector<bool> fits;
  if (!variable_length && data.size() <= kClassicPayloadBytes) {
    if (msg.parse_signals(data, values) != Libdbc::Message::ParseSignalsStatus::Success) {
      return out;
    }
  } else {
    values.reserve(signals.size());
    for (const SignalMeta& meta : signals) {
      const auto raw = extractRawBits(data, meta);
      if (!raw && !variable_length) {
        return out;
      }
      values.push_back(raw ? physicalValue(*raw, meta) : 0.0);
      if (variable_length) {
        fits.push_back(raw.has_value());
      }
    }
    if (variable_length && std::find(fits.begin(), fits.end(), true) == fits.end()) {
      return out;
    }
  }
  result = DecodeResult::kDecoded;

  const std::size_t count = signals.size() < values.size() ? signals.size() : values.size();
  out.reserve(count);
  for (std::size_t k = 0; k < count; ++k) {
    if (!fits.empty() && !fits[k]) {
      continue;
    }
    const SignalMeta& meta = signals[k];
    DecodedSignal decoded{.name = meta.name, .value = values[k], .unit = meta.unit};
    if (!meta.labels.empty()) {
      decoded.label_name = meta.label_name;
      // Inverse of `value = raw * factor + offset`; factor == 0 has no labels (see buildLabels).
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
