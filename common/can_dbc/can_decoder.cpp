#include <exception>
#include <libdbc/dbc.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace mf4_detail {

namespace {
/// DBC extended-frame flag (Vector convention: bit 31 set on 29-bit messages).
constexpr std::uint32_t kExtendedFlag = 0x8000'0000u;
}  // namespace

struct CanDecoder::Impl {
  std::vector<Libdbc::Message> messages;
  // Keyed by the DBC message id exactly as stored (raw, possibly with the
  // extended flag). Lookup handles both the raw and Vector-flagged forms.
  std::unordered_map<std::uint32_t, std::size_t> id_to_index;

  void addFrom(const Libdbc::DbcParser& parser) {
    for (const auto& msg : parser.get_messages()) {
      id_to_index[msg.id()] = messages.size();
      messages.push_back(msg);
    }
  }

  const Libdbc::Message* find(std::uint32_t can_id, bool extended) const {
    if (extended) {
      // Prefer the Vector-flagged 29-bit message so an extended frame is not
      // shadowed by a standard message sharing the same numeric id; fall back to
      // the raw id for DBCs that store extended messages without the flag.
      auto it = id_to_index.find(can_id | kExtendedFlag);
      if (it == id_to_index.end()) {
        it = id_to_index.find(can_id);
      }
      return it == id_to_index.end() ? nullptr : &messages[it->second];
    }
    const auto it = id_to_index.find(can_id);
    return it == id_to_index.end() ? nullptr : &messages[it->second];
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
    return PJ::unexpected(std::string("mf4: DBC parse error: ") + err.what());
  }
  impl_->addFrom(parser);
  return PJ::okStatus();
}

PJ::Status CanDecoder::loadDbcFile(const std::string& path) {
  Libdbc::DbcParser parser;
  try {
    parser.parse_file(path);
  } catch (const std::exception& err) {
    return PJ::unexpected(std::string("mf4: DBC parse error (") + path + "): " + err.what());
  }
  impl_->addFrom(parser);
  return PJ::okStatus();
}

std::size_t CanDecoder::messageCount() const {
  return impl_->messages.size();
}

std::string CanDecoder::messageName(std::uint32_t can_id, bool extended) const {
  const Libdbc::Message* msg = impl_->find(can_id, extended);
  return msg != nullptr ? msg->name() : std::string{};
}

std::vector<DecodedSignal> CanDecoder::decode(
    std::uint32_t can_id, bool extended, const std::vector<std::uint8_t>& data, bool& matched) const {
  matched = false;
  std::vector<DecodedSignal> out;

  const Libdbc::Message* msg_ptr = impl_->find(can_id, extended);
  if (msg_ptr == nullptr) {
    return out;
  }
  matched = true;
  const Libdbc::Message& msg = *msg_ptr;

  // Reject frames shorter than the message: dbc_parser_cpp would zero-fill the
  // missing bytes and return "success" with silently wrong signal values.
  if (data.size() < static_cast<std::size_t>(msg.size())) {
    return out;  // matched, but the frame is truncated -> cannot decode
  }

  std::vector<double> values;
  if (msg.parse_signals(data, values) != Libdbc::Message::ParseSignalsStatus::Success) {
    return out;  // matched but undecodable
  }

  const std::vector<Libdbc::Signal> signals = msg.get_signals();
  const std::size_t count = signals.size() < values.size() ? signals.size() : values.size();
  out.reserve(count);
  for (std::size_t k = 0; k < count; ++k) {
    out.push_back(DecodedSignal{signals[k].name, values[k], signals[k].unit});
  }
  return out;
}

}  // namespace mf4_detail
