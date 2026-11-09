#pragma once

#include <fstream>
#include <ios>
#include <optional>
#include <string>

namespace can_stream {

/// Why the SocketCAN interface `iface` can no longer carry traffic, read from
/// `<sysfs_net_root>/<iface>/flags` (Linux sysfs, "/sys/class/net"; e.g. "0x1003"), or nullopt while it
/// is up. Qt's socketcan backend reports neither case: its read loop just stops seeing frames.
inline std::optional<std::string> socketCanLinkProblem(const std::string& sysfs_net_root, const std::string& iface) {
  std::ifstream file(sysfs_net_root + "/" + iface + "/flags");
  if (!file) {
    return "interface removed";
  }
  constexpr unsigned long kIffUp = 0x1;
  unsigned long flags = 0;
  if (file >> std::hex >> flags && (flags & kIffUp) == 0) {
    return "interface went down";
  }
  return std::nullopt;  // up, or flags unreadable: not a reason to end the session
}

}  // namespace can_stream
