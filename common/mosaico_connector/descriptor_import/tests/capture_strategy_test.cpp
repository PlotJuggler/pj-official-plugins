// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The rule an explicit Download applies to its identity's cache slot: fresh
// capture whenever the slot is free; a leased slot may only reuse the existing
// artifact for a bounded request.
#include <gtest/gtest.h>

#include "descriptor_import/artifact_capture.hpp"

namespace {

using mosaico::CaptureStrategy;
using mosaico::chooseCaptureStrategy;

TEST(CaptureStrategy, FreeSlotAlwaysCapturesFresh) {
  EXPECT_EQ(
      chooseCaptureStrategy(/*slot_free=*/true, /*existing=*/true, /*bounded=*/true), CaptureStrategy::kCaptureFresh);
  EXPECT_EQ(chooseCaptureStrategy(true, true, false), CaptureStrategy::kCaptureFresh);
  EXPECT_EQ(chooseCaptureStrategy(true, false, false), CaptureStrategy::kCaptureFresh);
}

TEST(CaptureStrategy, LeasedSlotReusesOnlyBoundedRequests) {
  EXPECT_EQ(chooseCaptureStrategy(false, true, true), CaptureStrategy::kPromoteExisting);
  EXPECT_EQ(chooseCaptureStrategy(false, true, false), CaptureStrategy::kEagerOnly);  // open-ended: may differ
  EXPECT_EQ(chooseCaptureStrategy(false, false, true), CaptureStrategy::kEagerOnly);  // contended, nothing valid
}

}  // namespace
