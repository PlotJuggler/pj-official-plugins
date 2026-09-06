// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "pj_base/sdk/text_utils.hpp"
#include "pj_query/evaluator.hpp"

namespace PJ::query {

struct FilterSequence {
  std::string name;
  std::optional<std::int64_t> min_ts_ns;
  std::optional<std::int64_t> max_ts_ns;
  Metadata metadata;
};

struct FilterParams {
  std::string name_filter;
  bool name_regex = false;
  std::string query_text;
  std::optional<std::int64_t> date_from_ns;
  std::optional<std::int64_t> date_to_ns;
};

/// Name AND date AND metadata query. Empty/invalid metadata queries contribute
/// no exclusion; invalid name regexes match nothing. Date ranges intersect
/// inclusively; a dateless row is hidden when a date constraint is active.
/// Unknown date bounds are nullopt; zero is an ordinary epoch timestamp.
/// Row indices address the supplied vector only (no stable-row identity).
[[nodiscard]] inline std::vector<int> computeVisibleSequences(
    const std::vector<FilterSequence>& sequences, const FilterParams& params, const Schema& /*schema*/,
    Evaluator& evaluator) {
  const bool apply_query = !params.query_text.empty() && evaluator.validate(params.query_text).valid;
  const Query parsed_query(apply_query ? params.query_text : "");
  std::optional<std::regex> name_regex;
  if (params.name_regex && !params.name_filter.empty()) {
    try {
      name_regex.emplace(params.name_filter, std::regex::icase | std::regex::ECMAScript);
    } catch (const std::regex_error&) {
      return {};
    }
  }
  const std::string name_filter_lower = PJ::sdk::lowerAscii(params.name_filter);
  std::vector<int> visible;
  for (std::size_t index = 0; index < sequences.size(); ++index) {
    const auto& sequence = sequences[index];
    if (name_regex) {
      if (!std::regex_search(sequence.name, *name_regex)) {
        continue;
      }
    } else if (
        !name_filter_lower.empty() && PJ::sdk::lowerAscii(sequence.name).find(name_filter_lower) == std::string::npos) {
      continue;
    }
    if ((!sequence.min_ts_ns && !sequence.max_ts_ns && (params.date_from_ns || params.date_to_ns)) ||
        (params.date_from_ns && sequence.max_ts_ns && *sequence.max_ts_ns < *params.date_from_ns) ||
        (params.date_to_ns && sequence.min_ts_ns && *sequence.min_ts_ns > *params.date_to_ns)) {
      continue;
    }
    if (apply_query && !evaluator.evaluate(parsed_query, sequence.metadata)) {
      continue;
    }
    visible.push_back(static_cast<int>(index));
  }
  return visible;
}

}  // namespace PJ::query
