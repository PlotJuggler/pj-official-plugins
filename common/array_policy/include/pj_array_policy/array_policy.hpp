/**
 * @file array_policy.hpp
 * @brief Shared contract for the "maximum array size + clamp/skip" option
 *        that message-parser plugins apply when an incoming array field
 *        exceeds a configured length.
 *
 * Parsers flatten each array element into its own scalar series
 * (field[0], field[1], ...), so an unbounded array becomes an unbounded
 * number of series. ArrayLimit caps that: max_size is the threshold and
 * policy decides what happens past it — Clamp keeps the first max_size
 * elements, Skip drops the whole field.
 *
 * Before this contract each parser re-implemented the option with its own
 * member names and JSON keys (clamp_large_arrays vs discard_large_arrays).
 * arrayLimitFromJson() reads the canonical keys and falls back to the
 * legacy ones so configs and layouts written by older plugins keep working.
 *
 * This header lives in pj-official-plugins because every consumer is a
 * plugin in this repository; no component of plotjuggler_sdk references it.
 * Third-party plugins that want to be cross-compatible only need to honor
 * the JSON keys documented below — they do not have to link this header.
 */
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace pj::array_policy {

/// What a parser does with an array field longer than ArrayLimit::max_size.
enum class ArrayPolicy : uint8_t {
  /// Keep the first max_size elements; drop the rest. The series still exist,
  /// truncated.
  kClamp,
  /// Drop the entire field; no series is created for it.
  kSkip,
};

/// The unified array-size policy carried in a parser's config. Defaults are
/// canonical across every parser: clamp the first 500 elements.
struct ArrayLimit {
  /// Maximum number of array elements materialized per field. 0 = unlimited
  /// (no element is ever dropped, whatever the policy).
  uint32_t max_size = 500;
  /// Action taken when an array exceeds max_size.
  ArrayPolicy policy = ArrayPolicy::kClamp;

  /// True when oversized arrays are truncated to max_size (vs skipped whole).
  /// Convenience for parsers whose flatten helpers take a `clamp` bool.
  [[nodiscard]] bool clamp() const {
    return policy == ArrayPolicy::kClamp;
  }
};

/// Canonical JSON keys. Parsers should read/write these.
inline constexpr const char* kMaxArraySizeKey = "max_array_size";
inline constexpr const char* kArrayPolicyKey = "array_policy";  // "clamp" | "skip"

/// Reads an ArrayLimit from a parser config object.
///
/// Precedence:
///   1. Canonical keys: "max_array_size" (uint) + "array_policy" ("clamp"|"skip").
///   2. Legacy fallback for configs written before this contract:
///        "discard_large_arrays" (bool, true -> Skip)
///        "clamp_large_arrays"   (bool, true -> Clamp, false -> Skip)
///   3. Defaults (max_size = 500, policy = Clamp) for anything absent.
///
/// Never throws: a non-object config or a malformed field falls back to the
/// default for that field.
[[nodiscard]] inline ArrayLimit arrayLimitFromJson(const nlohmann::json& cfg) {
  ArrayLimit limit;
  if (!cfg.is_object()) {
    return limit;
  }

  limit.max_size = cfg.value(kMaxArraySizeKey, limit.max_size);

  if (auto it = cfg.find(kArrayPolicyKey); it != cfg.end() && it->is_string()) {
    limit.policy = (it->get<std::string>() == "skip") ? ArrayPolicy::kSkip : ArrayPolicy::kClamp;
  } else if (cfg.contains("discard_large_arrays")) {
    limit.policy = cfg.value("discard_large_arrays", false) ? ArrayPolicy::kSkip : ArrayPolicy::kClamp;
  } else if (cfg.contains("clamp_large_arrays")) {
    limit.policy = cfg.value("clamp_large_arrays", true) ? ArrayPolicy::kClamp : ArrayPolicy::kSkip;
  }
  return limit;
}

/// Writes the canonical keys for an ArrayLimit into a config object. Also
/// mirrors the legacy bools so a plugin .so built before this contract can
/// still honor the policy.
inline void arrayLimitToJson(nlohmann::json& cfg, const ArrayLimit& limit) {
  cfg[kMaxArraySizeKey] = limit.max_size;
  cfg[kArrayPolicyKey] = limit.clamp() ? "clamp" : "skip";
  cfg["clamp_large_arrays"] = limit.clamp();
  cfg["discard_large_arrays"] = !limit.clamp();
}

}  // namespace pj::array_policy
