// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The headless credential origin guard: MOSAICO_API_KEY is released to a
// descriptor target ONLY when MOSAICO_URL is set and origin-matches it. Any
// widening here silently hands the env bearer token to whatever server a
// hostile layout names, so every rejected shape is pinned. Hermetic via
// setenv (POSIX-only, like the ingest-core env tests).
#include "credential_resolve.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

#if !defined(_WIN32)

namespace {

using mosaico::envKeyAllowedForTarget;
using mosaico::resolveCredentials;
using mosaico::resolveHeadlessCredentials;

constexpr const char* kTarget = "grpc+tls://demo.mosaico.dev:6726";

struct EnvGuard {
  EnvGuard(const char* url, const char* key) {
    if (url != nullptr) {
      ::setenv("MOSAICO_URL", url, 1);
    } else {
      ::unsetenv("MOSAICO_URL");
    }
    if (key != nullptr) {
      ::setenv("MOSAICO_API_KEY", key, 1);
    } else {
      ::unsetenv("MOSAICO_API_KEY");
    }
  }
  ~EnvGuard() {
    ::unsetenv("MOSAICO_URL");
    ::unsetenv("MOSAICO_API_KEY");
  }
};

TEST(EnvKeyOriginGuard, Matrix) {
  // Released: exact origin, and spelling variants that normalize to it.
  EXPECT_TRUE(envKeyAllowedForTarget(kTarget, "grpc+tls://demo.mosaico.dev:6726"));
  EXPECT_TRUE(envKeyAllowedForTarget(kTarget, "GRPC+TLS://Demo.Mosaico.DEV:6726/path"));
  // Withheld: no binding, different origin components, unparsable shapes.
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, ""));
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, "grpc+tls://demo.mosaico.dev:6727"));
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, "grpc+tls://evil.example.com:6726"));
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, "grpc://demo.mosaico.dev:6726"));  // scheme downgrade
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, "demo.mosaico.dev:6726"));
  EXPECT_FALSE(envKeyAllowedForTarget("not-a-uri", "grpc+tls://demo.mosaico.dev:6726"));
}

TEST(HeadlessCredentials, EnvKeyRequiresOriginBoundUrl) {
  // Unbound settings view: no stored per-server key, so the env tier decides.
  PJ::sdk::SettingsView view{};
  {
    EnvGuard env(nullptr, "msco_secret");
    EXPECT_TRUE(resolveHeadlessCredentials(view, kTarget).api_key.empty());
  }
  {
    EnvGuard env("grpc+tls://evil.example.com:6726", "msco_secret");
    EXPECT_TRUE(resolveHeadlessCredentials(view, kTarget).api_key.empty());
  }
  {
    EnvGuard env(kTarget, "msco_secret");
    EXPECT_EQ(resolveHeadlessCredentials(view, kTarget).api_key, "msco_secret");
  }
}

TEST(InteractiveCredentials, EnvFallbackStaysUnguarded) {
  // The panel's resolve keeps the unguarded env fallback: the user chose the
  // server there, so the key applies to whatever they typed (PJ3 parity).
  PJ::sdk::SettingsView view{};
  EnvGuard env(nullptr, "msco_secret");
  EXPECT_EQ(resolveCredentials(view, kTarget).api_key, "msco_secret");
}

}  // namespace

#endif  // !_WIN32
