// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "source_descriptor.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/source/origin.hpp>

namespace mosaico {

namespace {

constexpr const char* kKind = "mosaico.pull";

bool fail(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

// Origin validation is the SDK's strict schemeless host:port rule; adapt its
// Expected<void> to this parser's (bool, *error) convention.
bool validateOrigin(const std::string& origin, std::string* error) {
  const auto valid = PJ::sdk::source::validateSchemelessOrigin(origin);
  if (!valid) {
    return fail(error, valid.error());
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

bool takeNs(const nlohmann::json& obj, const char* field, std::int64_t* out, std::string* error) {
  std::string text;
  if (!takeString(obj, field, &text, error)) {
    return false;
  }
  const auto parsed = PJ::sdk::source::parseDecimalNs(text);
  if (!parsed.has_value()) {
    return fail(error, std::string(field) + " is not a decimal nanosecond string");
  }
  *out = *parsed;
  return true;
}

// The one typed-to-JSON projection used by both SDK-owned canonicalization
// and record embedding. nlohmann::json object keys serialize alphabetically
// at every depth, matching the pinned canonical bytes.
nlohmann::json descriptorJson(const SourceDescriptor& descriptor, bool include_presentation) {
  nlohmann::json request;
  request["end_ns"] = std::to_string(descriptor.end_ns);
  request["origin"] = descriptor.origin;
  request["sequence"] = descriptor.sequence;
  request["start_ns"] = std::to_string(descriptor.start_ns);
  request["topics"] = descriptor.topics;
  nlohmann::json root;
  root["kind"] = descriptor.kind;
  root["request"] = std::move(request);
  root["v"] = descriptor.version;
  if (include_presentation && !descriptor.display_name.empty()) {
    root["display_name"] = descriptor.display_name;
  }
  return root;
}

}  // namespace

const PJ::sdk::source::SourceDescriptorPolicy& sourceDescriptorPolicy() {
  using PJ::sdk::source::IdentityScheme;
  using PJ::sdk::source::SourceDescriptorPolicy;

  static const SourceDescriptorPolicy policy = [] {
    SourceDescriptorPolicy value;
    value.identity_fields = {"v", "kind", "request"};
    value.presentation_fields = {"display_name"};
    value.identity = IdentityScheme{"mosaico:v1:sha256/128:", 32};
    value.max_descriptor_bytes = kMaxDescriptorBytes;
    value.max_string_bytes = kMaxStringBytes;
    value.max_container_entries = kMaxTopics;
    return value;
  }();
  return policy;
}

std::optional<SourceDescriptor> parseSourceDescriptor(std::string_view json, std::string* error) {
  const auto parsed = PJ::sdk::source::parseSourceDescriptor(json, sourceDescriptorPolicy());
  if (!parsed) {
    fail(error, parsed.error());
    return std::nullopt;
  }
  const nlohmann::json& obj = *parsed;

  SourceDescriptor descriptor;

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
  descriptor.version = 1;

  if (!takeString(obj, "kind", &descriptor.kind, error)) {
    return std::nullopt;
  }
  if (descriptor.kind != kKind) {
    fail(error, "unsupported kind \"" + descriptor.kind + "\" (expected \"" + kKind + "\")");
    return std::nullopt;
  }

  const auto request_it = obj.find("request");
  if (request_it == obj.end() || !request_it->is_object()) {
    fail(error, "field \"request\" must be an object");
    return std::nullopt;
  }
  const nlohmann::json& request = *request_it;
  // Nested allowlist: the SDK guards only top-level keys, so an unknown key
  // inside "request" (the credential-smuggling hazard) is rejected here.
  for (const auto& item : request.items()) {
    const std::string& key = item.key();
    if (key != "origin" && key != "sequence" && key != "topics" && key != "start_ns" && key != "end_ns") {
      fail(error, "unknown field \"request." + key + "\"");
      return std::nullopt;
    }
  }

  if (!takeString(request, "origin", &descriptor.origin, error) || !validateOrigin(descriptor.origin, error)) {
    return std::nullopt;
  }
  if (!takeString(request, "sequence", &descriptor.sequence, error)) {
    return std::nullopt;
  }
  if (descriptor.sequence.empty()) {
    fail(error, "sequence must not be empty");
    return std::nullopt;
  }

  const auto topics_it = request.find("topics");
  if (topics_it == request.end() || !topics_it->is_array()) {
    fail(error, "field \"topics\" must be an array of strings");
    return std::nullopt;
  }
  descriptor.topics.clear();
  descriptor.topics.reserve(topics_it->size());
  for (const auto& entry : *topics_it) {
    if (!entry.is_string()) {
      fail(error, "field \"topics\" must be an array of strings");
      return std::nullopt;
    }
    descriptor.topics.push_back(entry.get<std::string>());
  }
  // The exact-replay contract: the descriptor always carries the explicit
  // topic list that was fetched (selection expands before capture), so an
  // empty list has no meaning here — there is no "empty = all" shorthand.
  if (descriptor.topics.empty()) {
    fail(error, "topics must contain at least one topic");
    return std::nullopt;
  }
  // Canonical bytes must be UNIQUE per request: production sorts, so the wire
  // format is strictly-ascending by contract and anything else is rejected
  // rather than re-sorted (a re-sort would let two byte streams share one
  // identity).
  for (std::size_t i = 0; i < descriptor.topics.size(); ++i) {
    if (descriptor.topics[i].empty()) {
      fail(error, "topics must not contain empty names");
      return std::nullopt;
    }
    if (i > 0) {
      if (descriptor.topics[i] == descriptor.topics[i - 1]) {
        fail(error, "topics must not contain duplicate names (\"" + descriptor.topics[i] + "\")");
        return std::nullopt;
      }
      if (descriptor.topics[i] < descriptor.topics[i - 1]) {
        fail(error, "topics must be sorted ascending (canonical byte uniqueness)");
        return std::nullopt;
      }
    }
  }

  if (!takeNs(request, "start_ns", &descriptor.start_ns, error) ||
      !takeNs(request, "end_ns", &descriptor.end_ns, error)) {
    return std::nullopt;
  }

  // display_name is optional presentation and the canonical form omits it —
  // a canonical string must itself parse (dedup/cache comparison feeds it
  // back in).
  if (obj.contains("display_name")) {
    if (!takeString(obj, "display_name", &descriptor.display_name, error)) {
      return std::nullopt;
    }
  }

  // "0"/"0" is the unbounded sentinel; any other end < start is nonsense.
  if (descriptor.end_ns < descriptor.start_ns) {
    fail(
        error,
        "end_ns " + std::to_string(descriptor.end_ns) + " is before start_ns " + std::to_string(descriptor.start_ns));
    return std::nullopt;
  }
  return descriptor;
}

std::optional<SourceDescriptor> makePullDescriptor(
    std::string origin, std::string sequence, std::vector<std::string> topics, std::int64_t start_ns,
    std::int64_t end_ns, std::string* error) {
  SourceDescriptor descriptor;
  descriptor.version = 1;
  descriptor.kind = kKind;
  descriptor.origin = std::move(origin);
  descriptor.sequence = std::move(sequence);
  descriptor.topics = std::move(topics);
  // The identity is a property of the topic SET: sort + dedup so two
  // selections of the same topics hash to one cache entry. (A recorded
  // descriptor replays its serialized order verbatim, so existing identities
  // keep matching.)
  std::sort(descriptor.topics.begin(), descriptor.topics.end());
  descriptor.topics.erase(std::unique(descriptor.topics.begin(), descriptor.topics.end()), descriptor.topics.end());
  descriptor.start_ns = start_ns;
  descriptor.end_ns = end_ns;
  // Validate through the one parser (allowlist, limits, origin hygiene, time
  // ordering) so every producer meets the same bar as a layout consumer.
  // STRICT dump here: a name that is not valid UTF-8 cannot be represented
  // without altering it, and a U+FFFD-substituted descriptor would name a
  // DIFFERENT request (and collide with a genuine replacement-char name) —
  // so it is rejected and the download simply stays uncacheable.
  std::string json;
  try {
    json = descriptorJson(descriptor, true).dump();
  } catch (const nlohmann::json::exception&) {
    fail(error, "descriptor field is not valid UTF-8");
    return std::nullopt;
  }
  return parseSourceDescriptor(json, error);
}

std::string canonicalSourceDescriptorJson(const SourceDescriptor& descriptor) {
  return PJ::sdk::source::canonicalSourceDescriptorJson(descriptorJson(descriptor, false), sourceDescriptorPolicy());
}

std::string descriptorIdentity(const SourceDescriptor& descriptor) {
  return PJ::sdk::source::sourceDescriptorIdentity(descriptorJson(descriptor, false), sourceDescriptorPolicy());
}

std::string toSourceDescriptorJson(const SourceDescriptor& descriptor) {
  // error_handler_t::replace: sequence/topic names are server-supplied bytes
  // and a strict dump() throws on invalid UTF-8; deterministic U+FFFD
  // replacement keeps the canonical-bytes property without ever throwing.
  return descriptorJson(descriptor, true)
      .dump(-1, ' ', /*ensure_ascii=*/false, nlohmann::json::error_handler_t::replace);
}

}  // namespace mosaico
