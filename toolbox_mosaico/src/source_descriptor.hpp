// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The canonical Mosaico source descriptor: the PUBLIC, allowlisted, versioned
// description of one Download that the host records with a dataset and may
// hand back to this provider for a headless re-import. It snapshots the EXACT
// resolved request — the lowercased host:port origin, the sequence name, the
// explicit topic list actually fetched (selection always expands to explicit
// names before capture; never an "empty = all" shorthand), and the resolved
// absolute time window. The canonical serialization + identity are a contract
// pinned by docs/source-descriptor-vectors.json — changing either requires
// bumping the descriptor version, never silently regenerating vectors. No
// secrets, ever: api keys, cert paths and allow-insecure flags are rejected
// by the field allowlist.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <pj_base/sdk/descriptor_import/source_descriptor.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace mosaico {

/// Wire shape (v1, kind "mosaico.pull"):
///   {"kind":"mosaico.pull","request":{"end_ns":"…","origin":"host:port",
///    "sequence":"…","start_ns":"…","topics":[…]},"v":1}
/// plus an optional top-level "display_name" (presentation only, excluded
/// from the identity). Timestamps are decimal strings on the wire — int64
/// values survive JSON without double rounding — parsed to int64 here.
/// "0"/"0" = unbounded (the fetch's no-time-bounds fallback); end_ns is
/// EXCLUSIVE, exactly as resolved at fetch time.
struct SourceDescriptor {
  int version = 1;                  // "v"
  std::string kind;                 // "mosaico.pull"
  std::string origin;               // lowercase host:port, no scheme/userinfo/path
  std::string sequence;             // sequence NAME (Mosaico has no sequence UUIDs)
  std::vector<std::string> topics;  // explicit list, sorted ascending, >= 1
  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
  std::string display_name;  // EXCLUDED from identity
};

/// Hard validation limits (resource guard, layer 1).
inline constexpr std::size_t kMaxDescriptorBytes = 64 * 1024;
inline constexpr std::size_t kMaxTopics = 4096;
inline constexpr std::size_t kMaxStringBytes = 4096;

/// The SDK policy that owns Mosaico's descriptor field allowlist,
/// canonicalization, identity scheme and generic resource bounds.
[[nodiscard]] const PJ::sdk::descriptor_import::SourceDescriptorPolicy& sourceDescriptorPolicy();

/// Build a validated "mosaico.pull" descriptor from the resolved request
/// fields, stamping version/kind (their authority stays in this module).
/// Topics are sorted + deduped (the identity is a property of the topic SET).
/// The SAME validation as parseSourceDescriptor applies via a round-trip, so
/// an invalid request (malformed origin, empty topics, a name that is not
/// valid UTF-8, …) yields nullopt with a human-readable reason instead of an
/// uncacheable — or worse, request-aliasing — descriptor. Never throws.
[[nodiscard]] std::optional<SourceDescriptor> makePullDescriptor(
    std::string origin, std::string sequence, std::vector<std::string> topics, std::int64_t start_ns,
    std::int64_t end_ns, std::string* error);

/// Parse + validate. Rejects: wrong v/kind, missing/mistyped fields, unknown
/// fields at BOTH levels (allowlist!), over-limit sizes, a malformed origin
/// (uppercase, scheme, userinfo, path, IPv6 literal, missing/invalid port),
/// an empty sequence, an empty/duplicate-carrying/UNSORTED topic list (the
/// wire format is strictly ascending so canonical bytes stay unique per
/// request), non-decimal ns strings, and end < start (unless both 0). The version check compares the
/// JSON value exactly — never get<int>(), whose unchecked narrowing would let
/// v=2^32+1 alias v1. Error is human-readable.
[[nodiscard]] std::optional<SourceDescriptor> parseSourceDescriptor(std::string_view json, std::string* error);

/// Canonical serialization: the EXACT byte string the identity is computed
/// over. Alphabetical keys at every depth, compact (no whitespace), UTF-8,
/// ns as decimal strings, display_name OMITTED. Pinned by
/// docs/source-descriptor-vectors.json.
[[nodiscard]] std::string canonicalSourceDescriptorJson(const SourceDescriptor& descriptor);

/// "mosaico:v1:sha256/128:<32 lowercase hex>" over canonicalSourceDescriptorJson.
[[nodiscard]] std::string descriptorIdentity(const SourceDescriptor& descriptor);

/// Serialize for embedding/recording (canonical fields + display_name when
/// present). Uses deterministic U+FFFD replacement for invalid UTF-8, so it
/// never throws on server-supplied bytes.
[[nodiscard]] std::string toSourceDescriptorJson(const SourceDescriptor& descriptor);

}  // namespace mosaico
