#include <exception>
#include <libdbc/dbc.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace pj_can_dbc {

namespace {
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

/// Name/unit of one signal, cached at DBC load. Libdbc::Message::get_signals()
/// returns the whole Signal vector *by value* (each Signal owns strings plus
/// unused receiver/value-description vectors), so calling it per frame would
/// deep-copy on every decode; decode() only needs name + unit.
struct SignalMeta {
  std::string name;
  std::string unit;
};

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
        meta.push_back(SignalMeta{sig.name, sig.unit});
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

  const auto& signals = entry->signals;  // cached name/unit, no per-frame copy
  const std::size_t count = signals.size() < values.size() ? signals.size() : values.size();
  out.reserve(count);
  for (std::size_t k = 0; k < count; ++k) {
    out.push_back(DecodedSignal{signals[k].name, values[k], signals[k].unit});
  }
  return out;
}

}  // namespace pj_can_dbc
