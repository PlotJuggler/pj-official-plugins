// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The canonical Mosaico source descriptor: the PUBLIC, allowlisted, versioned
// description of one Download that a layout may embed and a provider may
// re-import. It snapshots the EXACT resolved request — sequence name, the
// explicit topic list actually fetched (All mode expands before capture;
// never an "empty = all" shorthand), and the resolved absolute time window
// (never proportional slider positions). The canonical serialization +
// identity are a contract pinned by docs/source-descriptor-vectors.json —
// changing either requires bumping the descriptor version, never silently
// regenerating vectors. No secrets, ever: api keys, cert paths and
// allow-insecure flags are rejected by the field allowlist.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mosaico {

/// Timestamps are decimal strings on the wire; parsed to int64 here.
/// "0"/"0" = unbounded (the fetch's no-time-bounds fallback). end_ns is
/// EXCLUSIVE, exactly as resolved at fetch time (a full-range selection
/// stores max_ts_ns + 1).
struct SourceDescriptor {
  int version = 1;                  // "v"
  std::string kind;                 // "mosaico-sequence"
  std::string server_uri;           // grpc+tls:// or grpc://
  std::string sequence;             // sequence NAME (Mosaico has no sequence UUIDs)
  std::vector<std::string> topics;  // explicit list, order preserved, >= 1
  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
  std::string display_name;  // EXCLUDED from identity
};

/// Hard validation limits (resource guard, layer 1).
inline constexpr std::size_t kMaxDescriptorBytes = 64 * 1024;
inline constexpr std::size_t kMaxTopics = 4096;
inline constexpr std::size_t kMaxStringBytes = 4096;

/// Parse + validate. Rejects: wrong v/kind, missing/mistyped fields, unknown
/// fields (allowlist!), over-limit sizes, non-grpc(+tls) scheme, URI userinfo
/// ('@' in the authority), URI query ('?') or fragment ('#'), an empty
/// sequence, an empty/duplicate-carrying topic list, non-numeric ns strings,
/// and end < start (unless both 0). Error is human-readable.
[[nodiscard]] std::optional<SourceDescriptor> parseSourceDescriptor(std::string_view json, std::string* error);

/// Canonical serialization: the EXACT byte string the identity is computed
/// over. Alphabetical keys, compact (no whitespace), UTF-8, ns as decimal
/// strings, display_name OMITTED. Pinned by docs/source-descriptor-vectors.json.
[[nodiscard]] std::string canonicalSourceDescriptorJson(const SourceDescriptor& d);

/// "mosaico:v1:sha256/128:<32 lowercase hex>" over canonicalSourceDescriptorJson.
[[nodiscard]] std::string descriptorIdentity(const SourceDescriptor& d);

/// Serialize for embedding in a layout (canonical fields + display_name).
[[nodiscard]] std::string toSourceDescriptorJson(const SourceDescriptor& d);

}  // namespace mosaico
