// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Tests for the shared visible-row helper (PJ::query::computeVisibleSequences)
// driven by the plugin's real Lua Engine — PJ3 validity gating + the
// name/date/query combination. Needs lua + sol2 only.

#include <pj_query/filter.hpp>

#include "gtest/gtest.h"
#include "query_engine.h"

namespace {

using mosaico::Engine;
using PJ::query::computeVisibleSequences;
using PJ::query::FilterParams;
using PJ::query::FilterSequence;
using PJ::query::Metadata;
using PJ::query::Schema;

// The shared filter treats nullopt as "no timestamp"; these rows are dateless.

std::vector<FilterSequence> sampleSequences() {
  return {
      {"seq_bonirob_cam", std::nullopt, std::nullopt, Metadata{{"robot", "bonirob"}, {"sensor", "camera"}}},
      {"seq_bonirob_laser", std::nullopt, std::nullopt, Metadata{{"robot", "bonirob"}, {"sensor", "laser"}}},
      {"seq_drone_cam", std::nullopt, std::nullopt, Metadata{{"robot", "drone"}, {"sensor", "camera"}}},
  };
}

Schema sampleSchema() {
  return Schema{
      {"robot", {"bonirob", "drone"}},
      {"sensor", {"camera", "laser"}},
  };
}

// ---------------------------------------------------------------------------
// computeVisibleSequences — PJ3 validity gating (TASK 2)
// ---------------------------------------------------------------------------

TEST(VisibleSequences, EmptyQueryShowsAll) {
  auto seqs = sampleSequences();
  FilterParams p;  // empty query, empty name filter
  Engine engine;
  auto vis = computeVisibleSequences(seqs, p, sampleSchema(), engine);
  EXPECT_EQ(vis, (std::vector<int>{0, 1, 2}));
}

TEST(VisibleSequences, ValidQueryFilters) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = "robot == \"bonirob\"";
  Engine engine;
  auto vis = computeVisibleSequences(seqs, p, sampleSchema(), engine);
  // Only the two bonirob sequences match.
  EXPECT_EQ(vis, (std::vector<int>{0, 1}));
}

TEST(VisibleSequences, ValidQueryWithShorthand) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = "sensor == \"camera\" or \"laser\"";  // shorthand → both sensors
  Engine engine;
  auto vis = computeVisibleSequences(seqs, p, sampleSchema(), engine);
  EXPECT_EQ(vis, (std::vector<int>{0, 1, 2}));
}

TEST(VisibleSequences, AndQueryNarrows) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = "robot == \"bonirob\" and sensor == \"camera\"";
  Engine engine;
  auto vis = computeVisibleSequences(seqs, p, sampleSchema(), engine);
  EXPECT_EQ(vis, (std::vector<int>{0}));
}

TEST(VisibleSequences, InvalidQueryDoesNotFilter) {
  // PJ3 parity: an invalid query must NOT hide rows (if (!valid) return;).
  // "robot ==" falls back to raw "robot ==" which is invalid Lua → all rows
  // stay visible (the query contributes no exclusion).
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = "robot ==";
  ASSERT_FALSE(Engine::validateText(p.query_text).valid);  // precondition
  Engine engine;
  auto vis = computeVisibleSequences(seqs, p, sampleSchema(), engine);
  EXPECT_EQ(vis, (std::vector<int>{0, 1, 2}));
}

TEST(VisibleSequences, SyntacticGarbageQueryDoesNotFilter) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = ")))";
  Engine engine;
  auto vis = computeVisibleSequences(seqs, p, sampleSchema(), engine);
  EXPECT_EQ(vis, (std::vector<int>{0, 1, 2}));
}

TEST(VisibleSequences, NameFilterCombinesWithQuery) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.name_filter = "laser";                // matches only seq_bonirob_laser
  p.query_text = "robot == \"bonirob\"";  // matches seq 0 and 1
  Engine engine;
  auto vis = computeVisibleSequences(seqs, p, sampleSchema(), engine);
  EXPECT_EQ(vis, (std::vector<int>{1}));
}

TEST(VisibleSequences, NameFilterStillAppliesWhenQueryInvalid) {
  // Invalid query → query contributes nothing, but the name filter still runs.
  auto seqs = sampleSequences();
  FilterParams p;
  p.name_filter = "drone";
  p.query_text = "this is not valid lua $$$";
  Engine engine;
  auto vis = computeVisibleSequences(seqs, p, sampleSchema(), engine);
  EXPECT_EQ(vis, (std::vector<int>{2}));
}

TEST(VisibleSequences, DateFilterExcludesOutOfRange) {
  std::vector<FilterSequence> seqs = {
      {"early", 1'000, 2'000, Metadata{{"robot", "a"}}},
      {"late", 10'000, 20'000, Metadata{{"robot", "b"}}},
  };
  FilterParams p;
  p.date_from_ns = 5'000;  // excludes "early" (max 2000 < 5000)
  Engine engine;
  auto vis = computeVisibleSequences(seqs, p, sampleSchema(), engine);
  EXPECT_EQ(vis, (std::vector<int>{1}));
}

}  // namespace
