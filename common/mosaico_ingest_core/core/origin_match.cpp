// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "origin_match.h"

#include <cctype>

namespace mosaico {

namespace {

std::string toLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

}  // namespace

std::optional<Origin> parseGrpcOrigin(std::string_view uri) {
  const std::size_t scheme_end = uri.find("://");
  if (scheme_end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string scheme = toLower(uri.substr(0, scheme_end));
  if (scheme != "grpc" && scheme != "grpc+tls") {
    return std::nullopt;
  }
  if (uri.find('?') != std::string_view::npos || uri.find('#') != std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view rest = uri.substr(scheme_end + 3);
  const std::size_t path_start = rest.find('/');
  const std::string_view authority = path_start == std::string_view::npos ? rest : rest.substr(0, path_start);
  if (authority.empty() || authority.find('@') != std::string_view::npos ||
      authority.find('[') != std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == authority.size()) {
    return std::nullopt;  // port is REQUIRED (no scheme-implied default)
  }
  const std::string_view port_text = authority.substr(colon + 1);
  std::uint32_t port = 0;
  for (const char c : port_text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    port = port * 10 + static_cast<std::uint32_t>(c - '0');
    if (port > 65535) {
      return std::nullopt;
    }
  }
  if (port == 0) {
    return std::nullopt;
  }
  Origin origin;
  origin.scheme = scheme;
  origin.host = toLower(authority.substr(0, colon));
  origin.port = static_cast<std::uint16_t>(port);
  return origin;
}

bool sameGrpcOrigin(std::string_view a, std::string_view b) {
  const auto origin_a = parseGrpcOrigin(a);
  const auto origin_b = parseGrpcOrigin(b);
  return origin_a.has_value() && origin_b.has_value() && origin_a->scheme == origin_b->scheme &&
         origin_a->host == origin_b->host && origin_a->port == origin_b->port;
}

}  // namespace mosaico
