// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace ros2_streamer {

/// Typesupport lookup needs the interface category (msg, action, or srv).
inline std::string interfaceTypeName(std::string_view type_namespace, std::string_view message_name) {
  std::string ns(type_namespace);
  for (size_t pos = ns.find("::"); pos != std::string::npos; pos = ns.find("::")) {
    ns.replace(pos, 2, "/");
  }
  return ns + "/" + std::string(message_name);
}

/// ROS .msg field names and parser specializations use package/Type.
inline std::string schemaTypeName(std::string_view interface_type) {
  const auto first = interface_type.find('/');
  if (first == std::string_view::npos) {
    return std::string(interface_type);
  }
  return std::string(interface_type.substr(0, first)) + std::string(interface_type.substr(interface_type.rfind('/')));
}

}  // namespace ros2_streamer
