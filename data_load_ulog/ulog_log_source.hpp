#pragma once

#include <string_view>
#include <utility>

namespace ulog_detail {

// PX4 starts a logged message with the emitting module in brackets: "[commander] Takeoff detected". Returns the module
// and the text after "] ", or an empty module and the whole message when it does not start that way (a bracketed
// word with spaces or nothing after it is part of the text).
inline std::pair<std::string_view, std::string_view> splitModulePrefix(std::string_view message) {
  const auto close = message.find(']');
  if (!message.starts_with('[') || close == std::string_view::npos || close < 2 || close + 2 >= message.size() ||
      message[close + 1] != ' ') {
    return {{}, message};
  }
  const auto module = message.substr(1, close - 1);
  if (module.find_first_of(" [") != std::string_view::npos) {
    return {{}, message};
  }
  return {module, message.substr(close + 2)};
}

}  // namespace ulog_detail
