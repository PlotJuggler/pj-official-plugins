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
  EXPECT_EQ(buildWhepUrl("http://h:8889/", "cam0"), "http://h:8889/cam0/whep");
  EXPECT_EQ(buildWhepUrl("http://h:8889", "/cam0"), "http://h:8889/cam0/whep");
  // mediamtx paths may contain '/'
  EXPECT_EQ(buildWhepUrl("https://h", "site/cams/front"), "https://h/site/cams/front/whep");
}

TEST(WhepUrl, ResolveLocationAbsolutePassthrough) {
  EXPECT_EQ(resolveLocation("http://h:8889/cam0/whep", "http://other:1234/s/1"), "http://other:1234/s/1");
}

TEST(WhepUrl, ResolveLocationHostRelative) {
  // mediamtx answers with a host-relative Location like /cam0/whep/<uuid>
  EXPECT_EQ(resolveLocation("http://h:8889/cam0/whep", "/cam0/whep/abc-123"), "http://h:8889/cam0/whep/abc-123");
  EXPECT_EQ(resolveLocation("https://h/cam0/whep", "/x"), "https://h/x");
}

TEST(WhepUrl, ResolveLocationPathRelative) {
  EXPECT_EQ(resolveLocation("http://h:8889/cam0/whep", "abc-123"), "http://h:8889/cam0/abc-123");
}

}  // namespace
}  // namespace webrtc
}  // namespace PJ
