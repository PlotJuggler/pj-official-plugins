// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace mosaico {

// Case-insensitive substring test (PJ3 default filter mode).
inline bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) {
    return true;
  }
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
    return std::tolower(a) == std::tolower(b);
  });
  return it != haystack.end();
}

// Name filter: case-insensitive substring, or std::regex when the
// regex toggle is on (PJ3 ".*" button parity). An empty pattern matches
// everything. An invalid regex matches nothing, so the user sees the filter
// "take effect" rather than silently falling back to substring.
inline bool nameMatches(const std::string& name, const std::string& pattern, bool regex) {
  if (pattern.empty()) {
    return true;
  }
  if (!regex) {
    return containsCaseInsensitive(name, pattern);
  }
  try {
    const std::regex re(pattern, std::regex::icase | std::regex::ECMAScript);
    return std::regex_search(name, re);
  } catch (const std::regex_error&) {
    return false;
  }
}

}  // namespace mosaico
