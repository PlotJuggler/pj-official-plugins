// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "pj_query/filter.hpp"

namespace PJ::query {
namespace {

// A recording evaluator proves the seam without installing an execution engine.
struct RecordingEvaluator : Evaluator {
  bool valid = true;
  int validations = 0;
  std::vector<Metadata> rows;
  std::string expanded;
  ValidationResult validate(std::string_view) override {
    ++validations;
    return {valid, valid ? "" : "invalid expression", 0, 0};
  }
  bool evaluate(const Query& query, const Metadata& metadata) override {
    expanded = query.expanded();
    rows.push_back(metadata);
    return metadata.contains("match");
  }
};

std::vector<FilterSequence> sequences() {
  return {
      {"early camera", 1000, 2000, {{"match", "yes"}}},
      {"late laser", 10000, 20000, {{"match", "yes"}}},
      {"late camera", 10000, 20000, {{"other", "no"}}},
      {"dateless", {}, {}, {}}};
}

TEST(QueryFilter, EmptyAndInvalidQueriesDoNotExcludeButNameStillDoes) {
  RecordingEvaluator evaluator;
  FilterParams params;
  EXPECT_EQ(computeVisibleSequences(sequences(), params, {}, evaluator), (std::vector<int>{0, 1, 2, 3}));
  EXPECT_EQ(evaluator.validations, 0);
  evaluator.valid = false;
  for (const char* invalid : {"robot ==", ")))", "this is not valid lua $$$"}) {
    params.query_text = invalid;
    EXPECT_EQ(computeVisibleSequences(sequences(), params, {}, evaluator), (std::vector<int>{0, 1, 2, 3}));
  }
  params.name_filter = "LASER";
  EXPECT_EQ(computeVisibleSequences(sequences(), params, {}, evaluator), (std::vector<int>{1}));
  EXPECT_TRUE(evaluator.rows.empty());
}

TEST(QueryFilter, QueryReceivesExpansionAndFreshMetadataPerRow) {
  RecordingEvaluator evaluator;
  FilterParams params;
  params.query_text = "sensor == \"camera\" or \"laser\"";
  EXPECT_EQ(computeVisibleSequences(sequences(), params, {}, evaluator), (std::vector<int>{0, 1}));
  EXPECT_EQ(evaluator.expanded, "sensor == \"camera\" or sensor == \"laser\"");
  ASSERT_EQ(evaluator.rows.size(), 4u);
  EXPECT_TRUE(evaluator.rows.back().empty());
  params.name_filter = "laser";
  EXPECT_EQ(computeVisibleSequences(sequences(), params, {}, evaluator), (std::vector<int>{1}));
  params.date_to_ns = 9000;
  EXPECT_TRUE(computeVisibleSequences(sequences(), params, {}, evaluator).empty());
}

TEST(QueryFilter, DateIntersectionAndRegexCompose) {
  RecordingEvaluator evaluator;
  FilterParams params;
  params.date_from_ns = 5000;
  EXPECT_EQ(computeVisibleSequences(sequences(), params, {}, evaluator), (std::vector<int>{1, 2}));
  params.date_to_ns = 10000;  // inclusive intersection
  params.name_regex = true;
  params.name_filter = "CAM.*";
  EXPECT_EQ(computeVisibleSequences(sequences(), params, {}, evaluator), (std::vector<int>{2}));
  params.name_filter = "[";
  EXPECT_TRUE(computeVisibleSequences(sequences(), params, {}, evaluator).empty());
  params = {};
  params.date_from_ns = 0;  // actual epoch, not an unset sentinel
  EXPECT_EQ(
      computeVisibleSequences({{"epoch", 0, 0, {}}, {"unknown", {}, {}, {}}}, params, {}, evaluator),
      (std::vector<int>{0}));
}

}  // namespace
}  // namespace PJ::query
