// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The strict fail-closed grpc origin parse that trust and credential-release
// decisions ride on: every rejected shape must stay rejected (a parse that
// starts accepting userinfo or bracketed IPv6 silently widens what an env
// key can be released to).
#include "core/origin_match.h"

#include <gtest/gtest.h>

namespace {

using mosaico::parseGrpcOrigin;
using mosaico::sameGrpcOrigin;

TEST(OriginMatch, ParsesCanonicalTlsUri) {
  const auto origin = parseGrpcOrigin("grpc+tls://demo.mosaico.dev:6726");
  ASSERT_TRUE(origin.has_value());
  EXPECT_EQ(origin->scheme, "grpc+tls");
  EXPECT_EQ(origin->host, "demo.mosaico.dev");
  EXPECT_EQ(origin->port, 6726);
}

TEST(OriginMatch, LowercasesSchemeAndHostAndIgnoresPath) {
  const auto origin = parseGrpcOrigin("GRPC+TLS://Demo.Mosaico.DEV:6726/some/path");
  ASSERT_TRUE(origin.has_value());
  EXPECT_EQ(origin->scheme, "grpc+tls");
  EXPECT_EQ(origin->host, "demo.mosaico.dev");
  EXPECT_EQ(origin->port, 6726);
}

TEST(OriginMatch, PlaintextSchemeIsDistinctFromTls) {
  ASSERT_TRUE(parseGrpcOrigin("grpc://host:1").has_value());
  EXPECT_FALSE(sameGrpcOrigin("grpc://host:1", "grpc+tls://host:1"));
}

TEST(OriginMatch, RejectedShapes) {
  EXPECT_FALSE(parseGrpcOrigin("").has_value());
  EXPECT_FALSE(parseGrpcOrigin("demo.mosaico.dev:6726").has_value());          // no scheme
  EXPECT_FALSE(parseGrpcOrigin("https://demo.mosaico.dev:6726").has_value());  // wrong scheme
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://demo.mosaico.dev").has_value());    // port required
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://demo.mosaico.dev:").has_value());
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://:6726").has_value());  // empty host
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://user@host:6726").has_value());
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://host:6726?query=1").has_value());
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://host:6726#frag").has_value());
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://[::1]:6726").has_value());  // IPv6 unsupported
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://host:0").has_value());      // port range
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://host:65536").has_value());
  EXPECT_FALSE(parseGrpcOrigin("grpc+tls://host:12ab").has_value());
}

TEST(OriginMatch, SameOriginComparisons) {
  EXPECT_TRUE(sameGrpcOrigin("grpc+tls://Host:6726", "grpc+tls://host:6726/path"));
  EXPECT_FALSE(sameGrpcOrigin("grpc+tls://host:6726", "grpc+tls://host:6727"));
  EXPECT_FALSE(sameGrpcOrigin("grpc+tls://a:1", "grpc+tls://b:1"));
  // Rejected shapes never match, not even themselves.
  EXPECT_FALSE(sameGrpcOrigin("host:1", "host:1"));
}

}  // namespace
