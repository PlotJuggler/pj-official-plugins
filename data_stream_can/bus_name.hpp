#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace can_stream {

/// The topic segment of an interface: "CAN/<bus>/<Message>". '/' and whitespace would split or blur it, so
/// they become '_'.
inline std::string busName(std::string_view interface_name) {
  std::string bus(interface_name);
  for (char& c : bus) {
    if (c == '/' || std::isspace(static_cast<unsigned char>(c)) != 0) {
      c = '_';
    }
  }
  return bus;
}

}  // namespace can_stream
