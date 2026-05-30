// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Locks in the PJ3-parity fetch-completion policy. The bug this guards against:
// the panel used to close on the FIRST topic's pullFinished, tearing down the
// worker mid-stream and dropping the rest of a multi-topic batch. The decision
// now happens once, in buildFetchSummary, after the whole batch lands.

#include "../src/fetch_summary.h"

#include <map>

#include "gtest/gtest.h"

using mosaico::buildFetchSummary;
using mosaico::summarizeErrors;
using Errors = std::map<std::string, int, std::less<>>;

TEST(FetchSummary, MultiTopicAllSucceedImportsAndClosesOnce) {
  // 3 topics, all ok. Must import + close exactly once (not per topic).
  auto s = buildFetchSummary(
      /*total=*/3, /*done=*/3, /*failed=*/0, /*imported_any=*/true,
      /*cancelling=*/false, Errors{});
  EXPECT_TRUE(s.should_close);
  EXPECT_TRUE(s.should_import);
  EXPECT_EQ(s.status_text, "Imported 3/3 topics");
  EXPECT_TRUE(s.error_summary.empty());
}

TEST(FetchSummary, PartialSuccessStillImportsAndCloses) {
  // 1 of 2 failed — PJ3 still imports what arrived and closes, surfacing errors.
  Errors errs{{"timeout", 1}};
  auto s = buildFetchSummary(2, 2, 1, /*imported_any=*/true, false, errs);
  EXPECT_TRUE(s.should_close);
  EXPECT_TRUE(s.should_import);
  EXPECT_EQ(s.status_text, "Imported 1/2 topics");
  EXPECT_EQ(s.error_summary, "timeout");
}

TEST(FetchSummary, AllFailedStaysOpenNoImport) {
  Errors errs{{"no data", 2}};
  auto s = buildFetchSummary(2, 2, 2, /*imported_any=*/false, false, errs);
  EXPECT_FALSE(s.should_close);
  EXPECT_FALSE(s.should_import);
  EXPECT_EQ(s.error_summary, "[2x] no data");
  EXPECT_EQ(s.status_text, "Fetch failed: [2x] no data");
}

TEST(FetchSummary, CancellingStaysOpenNoImport) {
  // Even with some bytes already imported, a cancel must not close or import.
  auto s = buildFetchSummary(3, 1, 0, /*imported_any=*/true, /*cancelling=*/true, Errors{});
  EXPECT_FALSE(s.should_close);
  EXPECT_FALSE(s.should_import);
  EXPECT_EQ(s.status_text, "Download cancelled");
}

TEST(FetchSummary, ErrorDedupCollapsesIdenticalMessages) {
  Errors errs{{"connection refused", 3}, {"bad schema", 1}};
  const std::string summary = summarizeErrors(errs);
  // Map ordering is lexicographic: "bad schema" before "connection refused".
  EXPECT_EQ(summary, "bad schema\n[3x] connection refused");
}

TEST(FetchSummary, EmptyErrorsYieldEmptySummary) {
  EXPECT_TRUE(summarizeErrors(Errors{}).empty());
}
