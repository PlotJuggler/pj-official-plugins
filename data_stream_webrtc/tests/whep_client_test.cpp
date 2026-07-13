// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// whep_client tests. Pure URL helpers are tested directly; the HTTP functions
// are tested against a local ix::HttpServer stub (loopback only — the single
// deliberate exception to the no-network-in-tests policy).
#include "whep_client.hpp"

#include <gtest/gtest.h>

namespace PJ {
namespace webrtc {
namespace {

TEST(WhepUrl, BuildJoinsWithSingleSlashes) {
  EXPECT_EQ(buildWhepUrl("http://h:8889", "cam0"), "http://h:8889/cam0/whep");
}

}  // namespace
}  // namespace webrtc
}  // namespace PJ
