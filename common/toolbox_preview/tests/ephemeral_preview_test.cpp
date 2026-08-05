// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "toolbox_preview/ephemeral_preview.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using toolbox_preview::EphemeralPreview;
using toolbox_preview::PreviewBackend;
using toolbox_preview::PreviewOverlay;

namespace {

// Counts how often the preview submits and reads back, so a test can assert the change-gate:
// re-submit only on intent change, re-read only on a data-revision change (or a re-submit).
class CountingBackend : public PreviewBackend {
 public:
  int submits = 0;
  int reads = 0;

  PJ::Expected<std::string> submitPreview(const std::string&) override {
    ++submits;
    return std::string("preview/topic");
  }
  PreviewOverlay readPreviewOverlay(const std::string&, double) override {
    ++reads;
    PreviewOverlay overlay;
    overlay.markers.resize(1);  // non-empty so the caller can tell a read happened
    return overlay;
  }
  void removePreview() override {}
};

}  // namespace

TEST(EphemeralPreviewTest, ReadBackScalesWithDataNotTicks) {
  CountingBackend backend;
  EphemeralPreview preview;
  std::string error;

  // First refresh: one submit, one read.
  (void)preview.refresh(backend, "rule", "src", 0.0, /*data_revision=*/1, &error);
  EXPECT_EQ(backend.submits, 1);
  EXPECT_EQ(backend.reads, 1);

  // Same rule/source AND same data revision (an idle 20 Hz tick): cache hit — no extra work.
  (void)preview.refresh(backend, "rule", "src", 0.0, /*data_revision=*/1, &error);
  (void)preview.refresh(backend, "rule", "src", 0.0, /*data_revision=*/1, &error);
  EXPECT_EQ(backend.submits, 1);
  EXPECT_EQ(backend.reads, 1);

  // Data changed (new commit → new revision): re-read the host-maintained output, NO re-submit.
  (void)preview.refresh(backend, "rule", "src", 0.0, /*data_revision=*/2, &error);
  EXPECT_EQ(backend.submits, 1);
  EXPECT_EQ(backend.reads, 2);

  // Rule edited: re-submit AND re-read, even though the data revision is unchanged.
  (void)preview.refresh(backend, "rule2", "src", 0.0, /*data_revision=*/2, &error);
  EXPECT_EQ(backend.submits, 2);
  EXPECT_EQ(backend.reads, 3);
}

TEST(EphemeralPreviewTest, EmptyRuleClearsAndRereadsWhenItReturns) {
  CountingBackend backend;
  EphemeralPreview preview;
  std::string error;

  (void)preview.refresh(backend, "rule", "src", 0.0, /*data_revision=*/5, &error);
  EXPECT_EQ(backend.reads, 1);

  // Empty rule: nothing submitted or read; overlay clears.
  const PreviewOverlay cleared = preview.refresh(backend, "", "src", 0.0, /*data_revision=*/5, &error);
  EXPECT_TRUE(cleared.markers.empty());
  EXPECT_EQ(backend.submits, 1);
  EXPECT_EQ(backend.reads, 1);

  // Rule returns at the SAME data revision: must re-read (the empty interlude reset the gate).
  (void)preview.refresh(backend, "rule", "src", 0.0, /*data_revision=*/5, &error);
  EXPECT_EQ(backend.reads, 2);
}
