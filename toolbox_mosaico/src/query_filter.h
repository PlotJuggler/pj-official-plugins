// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Visible-row computation for the Mosaico sequence table. Combines three
// independent filters — name (substring/regex), date/time range, and the Lua
// metadata query — into the set of visible row indices.
//
// PJ3 parity (toolbox_mosaico.cpp onQueryChanged): an INVALID metadata query
// does NOT filter. PJ3 gates with `if (!valid) return;`, leaving the prior
// visible set untouched; here, where the visible set is recomputed from
// scratch each call, that translates to "the query contributes no exclusion"
// — i.e. only the name/date filters apply. A valid, non-empty query is
// evaluated per sequence via the Lua engine.
//
// Pulled out of mosaico_dialog.cpp so the gating + filter-combination logic is
// unit-testable without Qt widgets, Arrow, or Flight.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "date_filter.h"
#include "name_filter.h"
#include "query/engine.h"
#include "query/query.h"
#include "types.h"

namespace mosaico {

// One sequence's filterable fields. Mirrors the subset of SequenceRecord the
// filters read; kept as a standalone struct so tests don't depend on the full
// dialog types.
struct FilterSequence {
  std::string name;
  std::int64_t min_ts_ns = 0;
  std::int64_t max_ts_ns = 0;
  Metadata metadata;
};

// All filter inputs other than the sequence list.
struct FilterParams {
  // Name filter (substring, or regex when name_regex is true).
  std::string name_filter;
  bool name_regex = false;

  // Lua metadata query (raw editor text). Empty ⇒ no query filtering.
  std::string query_text;

  // Date/time range (PJ3 SequencePicker). See date_filter.h for semantics.
  std::int64_t date_from_ns = 0;
  std::int64_t date_to_ns = 0;
};

// Compute the visible row indices for `sequences` under `params`.
//
// A row is visible iff it passes the name filter AND the date filter AND the
// metadata query. The query passes trivially when it is empty OR when it is
// syntactically INVALID (PJ3 validity-gating: a bad query never hides rows).
// A valid, non-empty query is evaluated per sequence with the Lua engine.
//
// `schema` is forwarded to Query for shorthand expansion; pass the union of
// metadata keys→values across the dataset (the engine tolerates gaps).
[[nodiscard]] inline std::vector<int> computeVisibleSequences(
    const std::vector<FilterSequence>& sequences, const FilterParams& params, const Schema& schema) {
  std::vector<int> visible;
  visible.reserve(sequences.size());

  // Validity-gate the query exactly like PJ3: empty is "no filter"; a valid
  // non-empty query is applied; an invalid one contributes no exclusion.
  const bool query_empty = params.query_text.empty();
  const bool query_valid = query_empty || Engine::validate(params.query_text).valid;
  const bool apply_query = !query_empty && query_valid;

  Query parsed_query;
  if (apply_query) {
    parsed_query = Query(params.query_text, schema);
  }

  // Reused across sequences; the sol::state is cheap to keep alive.
  Engine engine;

  for (std::size_t i = 0; i < sequences.size(); ++i) {
    const auto& seq = sequences[i];

    if (!nameMatches(seq.name, params.name_filter, params.name_regex)) {
      continue;
    }
    if (!dateFilterMatches(seq.min_ts_ns, seq.max_ts_ns, params.date_from_ns, params.date_to_ns)) {
      continue;
    }
    if (apply_query) {
      engine.set(seq.metadata);
      const bool lua_match = engine.eval(parsed_query);
      engine.clear(seq.metadata);
      if (!lua_match) {
        continue;
      }
    }
    visible.push_back(static_cast<int>(i));
  }

  return visible;
}

}  // namespace mosaico
