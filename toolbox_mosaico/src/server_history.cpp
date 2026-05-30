/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "server_history.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
constexpr const char* kGrpcScheme = "grpc://";
constexpr const char* kGrpcTlsScheme = "grpc+tls://";

std::string trimmed(const std::string& value) {
  constexpr const char* whitespace = " \t\n\r\f\v";
  const auto first = value.find_first_not_of(whitespace);
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(whitespace);
  return value.substr(first, last - first + 1);
}

std::string toLowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
}  // namespace

std::string normalizeServerKey(const std::string& uri) {
  std::string s = trimmed(uri);
  if (s.empty()) {
    return {};
  }

  if (s.rfind(kGrpcTlsScheme, 0) == 0) {
    s.erase(0, std::strlen(kGrpcTlsScheme));
  } else if (s.rfind(kGrpcScheme, 0) == 0) {
    s.erase(0, std::strlen(kGrpcScheme));
  }

  if (!s.empty() && s.back() == '/') {
    s.pop_back();
  }
  if (s.empty()) {
    return {};
  }

  const auto colon = s.find(':');
  if (colon == std::string::npos) {
    return toLowerAscii(s);
  }

  std::string host = toLowerAscii(s.substr(0, colon));
  const std::string rest = s.substr(colon);
  return host + rest;
}

std::vector<std::string> promoteToHead(const std::vector<std::string>& history, const std::string& key, int cap) {
  if (key.empty()) {
    return history;
  }
  if (cap <= 0) {
    return {};
  }

  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(cap));
  out.push_back(key);
  for (const auto& entry : history) {
    if (entry == key) {
      continue;
    }
    if (out.size() >= static_cast<std::size_t>(cap)) {
      break;
    }
    out.push_back(entry);
  }
  return out;
}
