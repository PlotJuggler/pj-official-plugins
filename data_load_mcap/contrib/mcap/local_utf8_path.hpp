#pragma once
// PJ4 local addition (not upstream; keep it and the call sites marked "PJ4 local patch" on any mcap re-vendor).
//
// File names reach this library as UTF-8. On Windows a narrow std::string given to fopen, CreateFileA,
// std::ifstream or std::filesystem::path is read in the ANSI code page, so a name with non-ASCII characters
// opens the wrong file or none. A path built from the UTF-8 bytes is opened through its wide form instead.
#include <filesystem>
#include <string>
#include <string_view>

namespace mcap {
namespace internal {

inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

}  // namespace internal
}  // namespace mcap
