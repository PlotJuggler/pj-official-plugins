#include "can_decoder.hpp"

#include <exception>
#include <libdbc/dbc.hpp>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace mf4_detail {

namespace {
/// CAN identifiers are matched on their raw 11/29-bit value; the DBC
/// extended-frame flag (bit 31) and any stray high bits are masked off both
/// here and when indexing loaded messages.
constexpr std::uint32_t kCanIdMask = 0x1FFF'FFFFu;
}  // namespace

struct CanDecoder::Impl {
  std::vector<Libdbc::Message> messages;
  std::unordered_map<std::uint32_t, std::size_t> id_to_index;  // masked id -> messages index

  void addFrom(const Libdbc::DbcParser& parser) {
    for (const auto& msg : parser.get_messages()) {
      const std::uint32_t key = msg.id() & kCanIdMask;
      id_to_index[key] = messages.size();
      messages.push_back(msg);
    }
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

std::vector<DecodedSignal> CanDecoder::decode(
    std::uint32_t can_id, const std::vector<std::uint8_t>& data, bool& matched) const {
  matched = false;
  std::vector<DecodedSignal> out;

  const auto it = impl_->id_to_index.find(can_id & kCanIdMask);
  if (it == impl_->id_to_index.end()) {
    return out;
  }
  matched = true;

  const Libdbc::Message& msg = impl_->messages[it->second];
  std::vector<double> values;
  if (msg.parse_signals(data, values) != Libdbc::Message::ParseSignalsStatus::Success) {
    return out;  // matched but undecodable (e.g. length mismatch)
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
