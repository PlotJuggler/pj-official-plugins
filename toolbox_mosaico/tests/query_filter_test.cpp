// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Tests for the visible-row helper (computeVisibleSequences — PJ3 validity
// gating + name/date/query combination). Pure logic extracted from
// mosaico_dialog.cpp; needs lua + sol2 only.

#include "query_filter.h"

#include "gtest/gtest.h"

namespace {

using mosaico::computeVisibleSequences;
using mosaico::FilterParams;
using mosaico::FilterSequence;

std::vector<FilterSequence> sampleSequences() {
  return {
      {"seq_bonirob_cam", 0, 0, Metadata{{"robot", "bonirob"}, {"sensor", "camera"}}},
      {"seq_bonirob_laser", 0, 0, Metadata{{"robot", "bonirob"}, {"sensor", "laser"}}},
      {"seq_drone_cam", 0, 0, Metadata{{"robot", "drone"}, {"sensor", "camera"}}},
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
  auto vis = computeVisibleSequences(seqs, p, sampleSchema());
  EXPECT_EQ(vis, (std::vector<int>{0, 1, 2}));
}

TEST(VisibleSequences, ValidQueryFilters) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = "robot == \"bonirob\"";
  auto vis = computeVisibleSequences(seqs, p, sampleSchema());
  // Only the two bonirob sequences match.
  EXPECT_EQ(vis, (std::vector<int>{0, 1}));
}

TEST(VisibleSequences, ValidQueryWithShorthand) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = "sensor == \"camera\" or \"laser\"";  // shorthand → both sensors
  auto vis = computeVisibleSequences(seqs, p, sampleSchema());
  EXPECT_EQ(vis, (std::vector<int>{0, 1, 2}));
}

TEST(VisibleSequences, AndQueryNarrows) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = "robot == \"bonirob\" and sensor == \"camera\"";
  auto vis = computeVisibleSequences(seqs, p, sampleSchema());
  EXPECT_EQ(vis, (std::vector<int>{0}));
}

TEST(VisibleSequences, InvalidQueryDoesNotFilter) {
  // PJ3 parity: an invalid query must NOT hide rows (if (!valid) return;).
  // "robot ==" falls back to raw "robot ==" which is invalid Lua → all rows
  // stay visible (the query contributes no exclusion).
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = "robot ==";
  ASSERT_FALSE(Engine::validate(p.query_text).valid);  // precondition
  auto vis = computeVisibleSequences(seqs, p, sampleSchema());
  EXPECT_EQ(vis, (std::vector<int>{0, 1, 2}));
}

TEST(VisibleSequences, SyntacticGarbageQueryDoesNotFilter) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.query_text = ")))";
  auto vis = computeVisibleSequences(seqs, p, sampleSchema());
  EXPECT_EQ(vis, (std::vector<int>{0, 1, 2}));
}

TEST(VisibleSequences, NameFilterCombinesWithQuery) {
  auto seqs = sampleSequences();
  FilterParams p;
  p.name_filter = "laser";                // matches only seq_bonirob_laser
  p.query_text = "robot == \"bonirob\"";  // matches seq 0 and 1
  auto vis = computeVisibleSequences(seqs, p, sampleSchema());
  EXPECT_EQ(vis, (std::vector<int>{1}));
}

TEST(VisibleSequences, NameFilterStillAppliesWhenQueryInvalid) {
  // Invalid query → query contributes nothing, but the name filter still runs.
  auto seqs = sampleSequences();
  FilterParams p;
  p.name_filter = "drone";
  p.query_text = "this is not valid lua $$$";
  auto vis = computeVisibleSequences(seqs, p, sampleSchema());
  EXPECT_EQ(vis, (std::vector<int>{2}));
}

TEST(VisibleSequences, DateFilterExcludesOutOfRange) {
  std::vector<FilterSequence> seqs = {
      {"early", 1'000, 2'000, Metadata{{"robot", "a"}}},
      {"late", 10'000, 20'000, Metadata{{"robot", "b"}}},
  };
  FilterParams p;
  p.date_from_ns = 5'000;  // excludes "early" (max 2000 < 5000)
  auto vis = computeVisibleSequences(seqs, p, sampleSchema());
  EXPECT_EQ(vis, (std::vector<int>{1}));
}

}  // namespace
