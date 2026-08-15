// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "source_descriptor.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>

#include "core/sha256.h"

namespace mosaico {

namespace {

constexpr const char* kKind = "mosaico-sequence";

// The complete field allowlist — a descriptor carrying ANYTHING else (a
// token, a cert path, an allow-insecure flag, a future field) is rejected
// outright rather than silently ignored: unknown fields are the
// credential-smuggling and forward-compat hazard the allowlist closes.
constexpr const char* kAllowedFields[] = {"v",      "kind",     "server_uri", "sequence",
                                          "topics", "start_ns", "end_ns",     "display_name"};

bool fail(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

// Strict decimal-digit parse of an epoch-nanosecond string ("0" = unset).
// No sign, no whitespace, no exponent — the wire format is decimal strings
// precisely so 64-bit values survive JSON without double rounding.
bool parseDecimalNs(const std::string& text, std::int64_t* out) {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const int digit = c - '0';
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      return false;  // would overflow int64
    }
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

// Minimal URI hygiene for the descriptor: reject userinfo, query, fragment
// outright; grpc+tls/grpc only. Origin comparison for the credential guard is
// a separate concern — this only enforces what makes a descriptor unsafe to
// embed in a shareable file.
bool validateServerUri(const std::string& uri, std::string* error) {
  std::size_t authority_start = 0;
  if (uri.rfind("grpc+tls://", 0) == 0) {
    authority_start = 11;
  } else if (uri.rfind("grpc://", 0) == 0) {
    authority_start = 7;
  } else {
    return fail(error, "server_uri scheme must be grpc+tls:// or grpc://");
  }
  if (uri.find('?') != std::string::npos) {
    return fail(error, "server_uri must not contain a query string");
  }
  if (uri.find('#') != std::string::npos) {
    return fail(error, "server_uri must not contain a fragment");
  }
  const std::size_t authority_end = uri.find('/', authority_start);
  const std::size_t authority_len = (authority_end == std::string::npos ? uri.size() : authority_end) - authority_start;
  if (uri.substr(authority_start, authority_len).find('@') != std::string::npos) {
    return fail(error, "server_uri must not contain userinfo");
  }
  return true;
}

// Require `field` to be a string no longer than kMaxStringBytes; copy it out.
bool takeString(const nlohmann::json& obj, const char* field, std::string* out, std::string* error) {
  const auto it = obj.find(field);
  if (it == obj.end()) {
    return fail(error, std::string("missing field \"") + field + "\"");
  }
  if (!it->is_string()) {
    return fail(error, std::string("field \"") + field + "\" must be a string");
  }
  *out = it->get<std::string>();
  if (out->size() > kMaxStringBytes) {
    return fail(
        error,
        std::string("field \"") + field + "\" exceeds the " + std::to_string(kMaxStringBytes) + "-byte string limit");
  }
  return true;
}

// Require `field` to be an array of <= max_entries strings, each within the
// string limit; copy it out.
bool takeStringArray(
    const nlohmann::json& obj, const char* field, std::size_t max_entries, std::vector<std::string>* out,
    std::string* error) {
  const auto it = obj.find(field);
  if (it == obj.end()) {
    return fail(error, std::string("missing field \"") + field + "\"");
  }
  if (!it->is_array()) {
    return fail(error, std::string("field \"") + field + "\" must be an array of strings");
  }
  if (it->size() > max_entries) {
    return fail(error, std::string(field) + " exceeds the " + std::to_string(max_entries) + "-entry limit");
  }
  out->clear();
  out->reserve(it->size());
  for (const auto& entry : *it) {
    if (!entry.is_string()) {
      return fail(error, std::string("field \"") + field + "\" must be an array of strings");
    }
    std::string value = entry.get<std::string>();
    if (value.size() > kMaxStringBytes) {
      return fail(
          error, std::string("field \"") + field + "\" entry exceeds the " + std::to_string(kMaxStringBytes) +
                     "-byte string limit");
    }
    out->push_back(std::move(value));
  }
  return true;
}

bool takeNs(const nlohmann::json& obj, const char* field, std::int64_t* out, std::string* error) {
  std::string text;
  if (!takeString(obj, field, &text, error)) {
    return false;
  }
  if (!parseDecimalNs(text, out)) {
    return fail(error, std::string(field) + " is not a decimal nanosecond string");
  }
  return true;
}

}  // namespace

std::optional<SourceDescriptor> parseSourceDescriptor(std::string_view json, std::string* error) {
  if (json.size() > kMaxDescriptorBytes) {
    fail(error, "descriptor exceeds the " + std::to_string(kMaxDescriptorBytes) + "-byte limit");
    return std::nullopt;
  }
  const auto obj = nlohmann::json::parse(json, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (obj.is_discarded()) {
    fail(error, "descriptor is not valid JSON");
    return std::nullopt;
  }
  if (!obj.is_object()) {
    fail(error, "descriptor is not a JSON object");
    return std::nullopt;
  }

  // Allowlist BEFORE field extraction: an unknown field is rejected even when
  // everything required is present and valid.
  for (const auto& [key, value] : obj.items()) {
    (void)value;
    bool allowed = false;
    for (const char* candidate : kAllowedFields) {
      if (key == candidate) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      fail(error, "unknown field \"" + key + "\"");
      return std::nullopt;
    }
  }

  SourceDescriptor d;

  const auto v_it = obj.find("v");
  if (v_it == obj.end()) {
    fail(error, "missing field \"v\"");
    return std::nullopt;
  }
  if (!v_it->is_number_integer()) {
    fail(error, "field \"v\" must be an integer");
    return std::nullopt;
  }
  // Exact-value comparison, never get<int>() — nlohmann's get<> is an
  // unchecked narrowing cast, so e.g. v=4294967297 (2^32+1) would narrow to 1
  // and alias a genuine v1 descriptor (fail-closed versioning).
  // json::operator== compares integer values exactly across int/uint width.
  if (*v_it != 1) {
    fail(error, "unsupported descriptor version " + v_it->dump() + " (expected 1)");
    return std::nullopt;
  }
  d.version = 1;

  if (!takeString(obj, "kind", &d.kind, error)) {
    return std::nullopt;
  }
  if (d.kind != kKind) {
    fail(error, "unsupported kind \"" + d.kind + "\" (expected \"" + kKind + "\")");
    return std::nullopt;
  }

  if (!takeString(obj, "server_uri", &d.server_uri, error) || !validateServerUri(d.server_uri, error)) {
    return std::nullopt;
  }
  if (!takeString(obj, "sequence", &d.sequence, error)) {
    return std::nullopt;
  }
  if (d.sequence.empty()) {
    fail(error, "sequence must not be empty");
    return std::nullopt;
  }
  if (!takeStringArray(obj, "topics", kMaxTopics, &d.topics, error)) {
    return std::nullopt;
  }
  // The exact-replay contract: the descriptor always carries the explicit
  // topic list that was fetched (All mode expands before capture), so an
  // empty list has no meaning here — unlike mcap-cloud, there is no
  // "empty = all" wire shorthand to fall back to.
  if (d.topics.empty()) {
    fail(error, "topics must contain at least one topic");
    return std::nullopt;
  }
  {
    std::vector<std::string> sorted_topics = d.topics;
    std::sort(sorted_topics.begin(), sorted_topics.end());
    for (std::size_t i = 0; i < sorted_topics.size(); ++i) {
      if (sorted_topics[i].empty()) {
        fail(error, "topics must not contain empty names");
        return std::nullopt;
      }
      if (i > 0 && sorted_topics[i] == sorted_topics[i - 1]) {
        fail(error, "topics must not contain duplicate names (\"" + sorted_topics[i] + "\")");
        return std::nullopt;
      }
    }
  }
  if (!takeNs(obj, "start_ns", &d.start_ns, error) || !takeNs(obj, "end_ns", &d.end_ns, error)) {
    return std::nullopt;
  }

  // display_name is optional: the canonical form omits it, and a canonical
  // string must itself parse (dedup/cache comparison feeds it back in).
  if (obj.contains("display_name")) {
    if (!takeString(obj, "display_name", &d.display_name, error)) {
      return std::nullopt;
    }
  }

  // "0"/"0" is the unbounded sentinel; any other end < start is nonsense.
  if (d.end_ns < d.start_ns) {
    fail(error, "end_ns " + std::to_string(d.end_ns) + " is before start_ns " + std::to_string(d.start_ns));
    return std::nullopt;
  }
  return d;
}

namespace {

// The canonical fields in ALPHABETICAL insert order — the ONE field list both
// serializations share, so a new descriptor field cannot land in only one of
// them. The insert order IS the vectors-file contract, never an artifact of
// map ordering.
void appendCanonicalFields(nlohmann::ordered_json& j, const SourceDescriptor& d) {
  j["end_ns"] = std::to_string(d.end_ns);
  j["kind"] = d.kind;
  j["sequence"] = d.sequence;
  j["server_uri"] = d.server_uri;
  j["start_ns"] = std::to_string(d.start_ns);
  j["topics"] = d.topics;
  j["v"] = d.version;
}

}  // namespace

std::optional<SourceDescriptor> makeSequenceDescriptor(
    std::string server_uri, std::string sequence, std::vector<std::string> topics, std::int64_t start_ns,
    std::int64_t end_ns, std::string display_name, std::string* error) {
  SourceDescriptor d;
  d.version = 1;
  d.kind = kKind;
  d.server_uri = std::move(server_uri);
  d.sequence = std::move(sequence);
  d.topics = std::move(topics);
  // The identity is a property of the topic SET: sort so two selections of
  // the same topics in different orders hash to one cache entry. (A layout
  // replays its serialized order verbatim, so existing identities keep
  // matching their own artifacts.)
  std::sort(d.topics.begin(), d.topics.end());
  d.start_ns = start_ns;
  d.end_ns = end_ns;
  d.display_name = std::move(display_name);
  // Validate through the one parser (allowlist, limits, URI hygiene, time
  // ordering) so every producer meets the same bar as a layout consumer.
  if (!parseSourceDescriptor(toSourceDescriptorJson(d), error).has_value()) {
    return std::nullopt;
  }
  return d;
}

std::string canonicalSourceDescriptorJson(const SourceDescriptor& d) {
  // display_name is deliberately absent (identity excludes it: a rename is
  // not a new request).
  nlohmann::ordered_json j;
  appendCanonicalFields(j, d);
  return j.dump();
}

std::string descriptorIdentity(const SourceDescriptor& d) {
  // sha256/128 = the first 128 bits (16 bytes -> 32 lowercase hex chars).
  return "mosaico:v1:sha256/128:" + sha256HexPrefix(canonicalSourceDescriptorJson(d), 16);
}

std::string toSourceDescriptorJson(const SourceDescriptor& d) {
  // Canonical fields + display_name, still in alphabetical insert order
  // ("display_name" sorts first).
  nlohmann::ordered_json j;
  j["display_name"] = d.display_name;
  appendCanonicalFields(j, d);
  return j.dump();
}

}  // namespace mosaico
