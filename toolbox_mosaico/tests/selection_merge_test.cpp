// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "selection_merge.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using mosaico::mergeReportedSelection;

std::function<bool(const std::string&)> hiddenSet(std::vector<std::string> hidden) {
  return [hidden = std::move(hidden)](const std::string& name) {
    return std::find(hidden.begin(), hidden.end(), name) != hidden.end();
  };
}

const std::vector<std::string> kKnown = {"alpha", "beta", "gamma", "delta"};

// The core contract: a selected name the filter hides survives a selection
// change made while it is out of view.
TEST(SelectionMerge, KeepsFilterHiddenSelection) {
  const auto next = mergeReportedSelection({"alpha", "beta"}, {"beta", "gamma"}, kKnown, hiddenSet({"alpha"}));
  EXPECT_EQ(next, (std::vector<std::string>{"alpha", "beta", "gamma"}));
}

// A visible name absent from the report was deselected by the user — dropped.
TEST(SelectionMerge, DropsDeselectedVisibleName) {
  const auto next = mergeReportedSelection({"alpha", "beta"}, {"beta"}, kKnown, hiddenSet({}));
  EXPECT_EQ(next, (std::vector<std::string>{"beta"}));
}

// Names not present in the current listing never enter the selection, whether
// they come from the report or from stale previous state.
TEST(SelectionMerge, BoundsToKnownNames) {
  const auto next = mergeReportedSelection({"ghost_prev"}, {"ghost", "alpha"}, kKnown, hiddenSet({"ghost_prev"}));
  EXPECT_EQ(next, (std::vector<std::string>{"alpha"}));
}

// A host that reports hidden-selected rows too must not produce duplicates.
TEST(SelectionMerge, DedupesHiddenAlsoReported) {
  const auto next = mergeReportedSelection({"alpha"}, {"alpha", "beta"}, kKnown, hiddenSet({"alpha"}));
  EXPECT_EQ(next, (std::vector<std::string>{"alpha", "beta"}));
}

// Clearing the visible selection keeps only what the filter hides.
TEST(SelectionMerge, EmptyReportKeepsOnlyHidden) {
  const auto next = mergeReportedSelection({"alpha", "beta"}, {}, kKnown, hiddenSet({"beta"}));
  EXPECT_EQ(next, (std::vector<std::string>{"beta"}));
}

}  // namespace
