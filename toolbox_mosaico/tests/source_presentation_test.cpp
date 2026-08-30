// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "source_presentation.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <pj_base/sdk/settings_store_host.hpp>
#include <string>

namespace {

using mosaico::SourceDescriptor;

class CountingSettingsBackend : public PJ::sdk::InMemorySettingsBackend {
 public:
  void setString(std::string_view key, std::string_view value) override {
    ++string_writes;
    InMemorySettingsBackend::setString(key, value);
  }

  int string_writes = 0;
};

struct Fixture {
  CountingSettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};

  PJ::sdk::SettingsView view() {
    return PJ::sdk::SettingsView{host.view()};
  }
};

SourceDescriptor descriptor() {
  SourceDescriptor d;
  d.server_uri = "grpc+tls://Demo.Mosaico.DEV:6726/private/path";
  d.sequence = "sequence-name";
  d.display_name = "Friendly sequence";
  return d;
}

std::string readString(PJ::sdk::SettingsBackend& backend, const std::string& key) {
  return backend.getString(key).value_or("");
}

TEST(SourcePresentation, DerivesQtCompatibleBase64UrlGroupWithoutPadding) {
  EXPECT_EQ(
      mosaico::sourcePresentationSettingsGroup("mosaico:v1:sha256/128:0123456789abcdef0123456789abcdef"),
      "source_presentation/v1/"
      "bW9zYWljbzp2MTpzaGEyNTYvMTI4OjAxMjM0NTY3ODlhYmNkZWYwMTIzNDU2Nzg5YWJjZGVm");
  EXPECT_EQ(mosaico::sourcePresentationSettingsGroup("id:\xc3\xa9"), "source_presentation/v1/aWQ6w6k");
}

TEST(SourcePresentation, WritesDisplayNameAndParsedHostPortOnly) {
  Fixture fx;
  const SourceDescriptor d = descriptor();
  const std::string identity = "mosaico:v1:sha256/128:0123456789abcdef0123456789abcdef";
  const std::string group = mosaico::sourcePresentationSettingsGroup(identity);

  mosaico::recordSourcePresentation(fx.view(), identity, d);

  EXPECT_EQ(readString(fx.backend, group + "/display_name"), "Friendly sequence");
  EXPECT_EQ(readString(fx.backend, group + "/origin"), "demo.mosaico.dev:6726");
  EXPECT_EQ(fx.backend.string_writes, 2);

  // Repeating an identical query is a read-only fast path.
  mosaico::recordSourcePresentation(fx.view(), identity, d);
  EXPECT_EQ(fx.backend.string_writes, 2);
}

TEST(SourcePresentation, FallsBackToSequenceAndCapsValuesAtTwoHundredCharacters) {
  Fixture fx;
  SourceDescriptor d = descriptor();
  d.display_name.clear();
  d.sequence = std::string(205, 's');
  d.server_uri = "grpc+tls://" + std::string(205, 'H') + ":6726";
  const std::string identity = "identity";
  const std::string group = mosaico::sourcePresentationSettingsGroup(identity);

  mosaico::recordSourcePresentation(fx.view(), identity, d);

  EXPECT_EQ(readString(fx.backend, group + "/display_name"), std::string(200, 's'));
  EXPECT_EQ(readString(fx.backend, group + "/origin"), std::string(200, 'h'));
}

TEST(SourcePresentation, EmptyIdentityWritesNothingAndUnparseableOriginStillWritesTheName) {
  Fixture fx;
  SourceDescriptor d = descriptor();

  mosaico::recordSourcePresentation(fx.view(), "", d);
  EXPECT_EQ(fx.backend.string_writes, 0);

  d.server_uri = "grpc+tls://missing-port";
  mosaico::recordSourcePresentation(fx.view(), "identity", d);

  const std::string group = mosaico::sourcePresentationSettingsGroup("identity");
  EXPECT_EQ(readString(fx.backend, group + "/display_name"), "Friendly sequence");
  EXPECT_FALSE(fx.backend.getString(group + "/origin").has_value());
  EXPECT_EQ(fx.backend.string_writes, 1);
}

}  // namespace
