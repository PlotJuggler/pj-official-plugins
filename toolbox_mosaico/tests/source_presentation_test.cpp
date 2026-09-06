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
  d.origin = "demo.mosaico.dev:6726";
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

TEST(SourcePresentation, WritesDisplayNameAndOrigin) {
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
  d.origin = std::string(205, 'h') + ":6726";
  const std::string identity = "identity";
  const std::string group = mosaico::sourcePresentationSettingsGroup(identity);

  mosaico::recordSourcePresentation(fx.view(), identity, d);

  EXPECT_EQ(readString(fx.backend, group + "/display_name"), std::string(200, 's'));
  EXPECT_EQ(readString(fx.backend, group + "/origin"), std::string(200, 'h'));
}

// Control/format code points in layout-supplied text must never reach the
// UI beside a trust verdict: C0/C1, zero-width and bidi controls are
// stripped, multi-byte code points survive.
TEST(SourcePresentation, StripsControlAndBidiFormatCodePoints) {
  Fixture fx;
  SourceDescriptor d = descriptor();
  d.display_name = std::string("A\x01") + "\xe2\x80\xae" +  // U+202E RLO
                   "B\xc3\xa9" + "\xe2\x80\x8b" +           // U+200B ZWSP
                   "\xd8\x9c" + "\xe2\x81\xa0";             // U+061C ALM, U+2060 WJ
  const std::string group = mosaico::sourcePresentationSettingsGroup("identity");

  mosaico::recordSourcePresentation(fx.view(), "identity", d);

  EXPECT_EQ(readString(fx.backend, group + "/display_name"), "AB\xc3\xa9");
}

TEST(SourcePresentation, EmptyIdentityWritesNothingAndEmptyOriginSkipsTheOriginKey) {
  Fixture fx;
  SourceDescriptor d = descriptor();

  mosaico::recordSourcePresentation(fx.view(), "", d);
  EXPECT_EQ(fx.backend.string_writes, 0);

  d.origin.clear();
  mosaico::recordSourcePresentation(fx.view(), "identity", d);

  const std::string group = mosaico::sourcePresentationSettingsGroup("identity");
  EXPECT_EQ(readString(fx.backend, group + "/display_name"), "Friendly sequence");
  EXPECT_FALSE(fx.backend.getString(group + "/origin").has_value());
  EXPECT_EQ(fx.backend.string_writes, 1);
}

}  // namespace
