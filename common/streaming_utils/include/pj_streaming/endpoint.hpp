// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pj::streaming {

[[nodiscard]] inline std::optional<uint16_t> parsePort(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  uint32_t value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [next, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || next != end || value == 0 || value > 65535) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(value);
}

/// Bracket an IPv6 literal when it is used as a URI authority. Already-
/// bracketed hosts and ordinary DNS/IPv4 addresses pass through unchanged.
[[nodiscard]] inline std::string authorityHost(std::string_view host) {
  if (host.find(':') != std::string_view::npos && !(host.starts_with('[') && host.ends_with(']'))) {
    return "[" + std::string(host) + "]";
  }
  return std::string(host);
}

[[nodiscard]] inline std::string composeEndpoint(
    std::string_view scheme, std::string_view host, std::string_view port = {}, std::string_view path = {}) {
  std::string result(scheme);
  if (!result.ends_with("://")) {
    result += "://";
  }
  result += authorityHost(host);
  if (!port.empty()) {
    result += ':';
    result += port;
  }
  if (!path.empty()) {
    if (!path.starts_with('/')) {
      result += '/';
    }
    result += path;
  }
  return result;
}

[[nodiscard]] inline std::string composeEndpoint(
    std::string_view scheme, std::string_view host, uint16_t port, std::string_view path = {}) {
  return composeEndpoint(scheme, host, std::to_string(port), path);
}

[[nodiscard]] inline std::string composeHostPort(std::string_view host, uint16_t port) {
  return authorityHost(host) + ":" + std::to_string(port);
}

}  // namespace pj::streaming
