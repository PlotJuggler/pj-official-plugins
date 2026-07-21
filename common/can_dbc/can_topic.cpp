#include <pj_can_dbc/can_topic.hpp>

namespace pj_can_dbc {

std::string hexId(std::uint32_t id) {
  static const char* const kHex = "0123456789ABCDEF";
  std::string out = "0x";
  bool started = false;
  for (int shift = 28; shift >= 0; shift -= 4) {
    const auto nibble = static_cast<std::size_t>((id >> shift) & 0xFu);
    if (nibble != 0 || started || shift == 0) {
      out.push_back(kHex[nibble]);
      started = true;
    }
  }
  return out;
}

std::string canTopicName(std::uint16_t bus_channel, const std::string& message_name, std::uint32_t can_id) {
  std::string out = "CAN/";
  if (bus_channel != 0) {
    out += "ch" + std::to_string(bus_channel) + "/";
  }
  out += message_name.empty() ? hexId(can_id) : message_name;
  return out;
}

}  // namespace pj_can_dbc
