/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>

// Returns true if every byte of `s` is printable ASCII (0x20–0x7E inclusive).
// Header/URI/cert/api-key values handed to gRPC must be pure printable ASCII:
// control bytes (CR, LF, NUL) are exactly what gRPC asserts-and-aborts on, so
// they are rejected at the earliest boundary (PJ3 main_window.cpp:947-963).
[[nodiscard]] inline bool isPrintableAscii(std::string_view s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

// Returns true if the key matches: msco_[32 lowercase alnum]_[8 hex chars]
[[nodiscard]] inline bool isValidApiKey(const std::string& key) {
  static const std::regex re(R"(^msco_[a-z0-9]{32}_[0-9a-f]{8}$)", std::regex::ECMAScript);
  return std::regex_match(key, re);
}

// Returns true if the file exists and is readable.
[[nodiscard]] inline bool isCertReadable(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  const std::filesystem::path p(path);
  if (!std::filesystem::exists(p)) {
    return false;
  }
  return std::ifstream(p).good();
}
