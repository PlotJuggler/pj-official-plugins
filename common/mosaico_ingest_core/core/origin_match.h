// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Strict grpc/grpc+tls origin parsing + comparison for trust and credential
// origin binding: the trusted-origin ledger keys entries by origin, and the
// MOSAICO_API_KEY env token may only apply to a target whose origin equals
// MOSAICO_URL's. Deliberately NOT normalizeServerKey() — that is a storage
// key (lossy, collision-tolerant), not an origin parser; a trust or
// credential-release decision needs the strict fail-closed parse below.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mosaico {

/// A parsed Mosaico server origin.
struct Origin {
  std::string scheme;  // "grpc" | "grpc+tls" (lowercase)
  std::string host;    // lowercase; no IDNA/punycode normalization
  std::uint16_t port;  // EXPLICIT only — gRPC has no scheme-implied default port
};

/// Strict fail-closed parse. Returns nullopt for: a non-grpc(+tls) scheme,
/// userinfo ('@' before the authority ends), query ('?'), fragment ('#'),
/// an empty host, a bracketed IPv6 literal (unsupported), or a missing /
/// unparsable / out-of-range port. Paths are allowed and ignored.
[[nodiscard]] std::optional<Origin> parseGrpcOrigin(std::string_view uri);

/// True iff both parse and all three fields match.
[[nodiscard]] bool sameGrpcOrigin(std::string_view a, std::string_view b);

}  // namespace mosaico
