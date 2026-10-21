// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// featuresDigest / featuresBlock (features_digest.hpp) and the built-in
// reference (fallback_features.hpp), rendered against the benchmark fixtures
// in docs/benchmarks/features/fixtures/ -- the same documents the eval runner
// (features_eval_test.cpp) hands the model.
#include "features_digest.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/feature_catalog_host.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fallback_features.hpp"
#include "session_text.hpp"
#include "support/file_io.hpp"

namespace {

using assistant_agent::fallbackFeatures;
using assistant_agent::featuresBlock;
using assistant_agent::featuresDigest;
using nlohmann::json;

constexpr std::size_t kUnlimited = 1000000;

std::string readFixture(const std::string& name) {
  return assistant_agent::testing::readFileToString(
      std::string(ASSISTANT_FEATURES_FIXTURES_DIR) + "/" + name + ".json");
}

json fixture(const std::string& name) {
  return json::parse(readFixture(name));
}

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    out.push_back(line);
  }
  return out;
}

// The one line naming `name` as a plugin ("- <name> ..."), or "".
std::string pluginLine(const std::string& text, const std::string& name) {
  for (const auto& line : lines(text)) {
    if (line.rfind("- " + name + " ", 0) == 0) {
      return line;
    }
  }
  return {};
}

// The heading a plugin line sits under: the nearest line above it that does
// not start with "- ".
std::string sectionOf(const std::string& text, const std::string& name) {
  std::string heading;
  for (const auto& line : lines(text)) {
    if (line.rfind("- ", 0) != 0) {
      heading = line;
    } else if (line.rfind("- " + name + " ", 0) == 0) {
      return heading;
    }
  }
  return {};
}

std::size_t count(const std::string& hay, const std::string& needle) {
  std::size_t n = 0;
  for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

constexpr const char* kFooter =
    "Plugins are installed from File → Extensions Marketplace…; a plugin not listed here may still be distributed "
    "outside the Marketplace.";

// --- Every fixture: shape and section order --------------------------------

TEST(FeaturesDigest, EveryFixtureRendersItsSectionsInOrderWithNoBlankLine) {
  for (const char* name : {"host_bundled", "host_offline", "host_disabled"}) {
    SCOPED_TRACE(name);
    const std::string text = featuresDigest(readFixture(name));
    EXPECT_EQ(text.rfind("PlotJuggler features (PlotJuggler 4.1.0, from the host):\n", 0), 0u) << text;
    EXPECT_EQ(text.find("\n\n"), std::string::npos) << "a blank line would end the block early for the strip";
    EXPECT_EQ(text.substr(text.size() - std::string(kFooter).size()), kFooter);

    const std::size_t core = text.find("\nBuilt in:\n");
    const std::size_t limits = text.find("\nKnown limitations:\n");
    const std::size_t loaded = text.find("\nPlugins loaded (installed and ready to use):\n");
    ASSERT_NE(core, std::string::npos);
    ASSERT_NE(limits, std::string::npos);
    ASSERT_NE(loaded, std::string::npos);
    EXPECT_LT(core, limits);
    EXPECT_LT(limits, loaded);
    const std::size_t installed = text.find("\nInstalled but not active:");
    ASSERT_NE(installed, std::string::npos);
    EXPECT_LT(loaded, installed);
    const std::size_t available = text.find("\nAvailable in the Marketplace (not installed):");
    if (std::string(name) == "host_offline") {
      EXPECT_EQ(available, std::string::npos) << "an unchecked Marketplace has no section, only the registry line";
    } else {
      ASSERT_NE(available, std::string::npos);
      EXPECT_LT(installed, available);
    }

    const json doc = fixture(name);
    for (const auto& e : doc["core"]) {
      EXPECT_NE(text.find("- " + e["name"].get<std::string>() + ": "), std::string::npos) << e["name"];
      EXPECT_NE(text.find("Start: " + e["entry_point"].get<std::string>()), std::string::npos) << e["name"];
    }
    for (const auto& e : doc["limitations"]) {
      EXPECT_NE(text.find("- " + e["summary"].get<std::string>()), std::string::npos) << e["id"];
    }
  }
}

TEST(FeaturesDigest, LoadedPluginsStartWhereTheyAreAndAreNeverOfferedForInstall) {
  const std::string text = featuresDigest(readFixture("host_bundled"));
  const json doc = fixture("host_bundled");
  for (const auto& e : doc["loaded"]) {
    const std::string name = e["name"].get<std::string>();
    SCOPED_TRACE(name);
    const std::string line = pluginLine(text, name);
    ASSERT_FALSE(line.empty());
    EXPECT_EQ(sectionOf(text, name), "Plugins loaded (installed and ready to use):");
    // "Install:" is this renderer's wording; a host summary may still
    // mention installing a companion (Mosaico names the Arrow parser).
    EXPECT_EQ(line.find("Install:"), std::string::npos) << line;
    EXPECT_EQ(line.find("Extensions Marketplace"), std::string::npos) << line;
    EXPECT_NE(line.find(e["version"].get<std::string>()), std::string::npos) << line;
  }
  EXPECT_EQ(
      pluginLine(text, "CSV Loader"),
      "- CSV Loader 1.0.7 (data loader): Data loader for CSV files. Start: Sources panel → File (or File → Open Data "
      "File...)");
}

TEST(FeaturesDigest, AParserGetsItsNoPanelSentenceInsteadOfAStartRoute) {
  const std::string line = pluginLine(featuresDigest(readFixture("host_bundled")), "JSON Parser");
  EXPECT_EQ(
      line,
      "- JSON Parser 1.3.0 (message parser): Message parser for JSON, CBOR, MessagePack, and BSON encoded data. No "
      "panel of its own: chosen as the message format inside the data source that uses it (for example a stream's "
      "message serialization selector).");

  // And without any entry point from the host, the same sentence.
  json doc = fixture("host_bundled");
  for (auto& e : doc["loaded"]) {
    if (e["id"] == "json-parser") {
      e.erase("entry_point");
    }
  }
  EXPECT_EQ(pluginLine(featuresDigest(doc.dump()), "JSON Parser"), line);
}

TEST(FeaturesDigest, InstallWordingAppearsOnlyForMarketplaceEntries) {
  const std::string text = featuresDigest(readFixture("host_bundled"));
  const json doc = fixture("host_bundled");
  EXPECT_EQ(count(text, "Install: File → Extensions Marketplace…"), doc["available"].size());
  for (const auto& e : doc["available"]) {
    const std::string name = e["name"].get<std::string>();
    SCOPED_TRACE(name);
    EXPECT_EQ(sectionOf(text, name), "Available in the Marketplace (not installed):");
    const std::string line = pluginLine(text, name);
    EXPECT_NE(
        line.find("Install: File → Extensions Marketplace… → search \"" + name + "\" → Install"), std::string::npos)
        << line;
  }
  EXPECT_EQ(
      pluginLine(text, "MQTT Subscriber"),
      "- MQTT Subscriber 0.10.0 (streaming source): Real-time data streaming via MQTT protocol. Subscribe to topics "
      "and visualize live data. Install: File → Extensions Marketplace… → search \"MQTT Subscriber\" → Install; after "
      "installing: Sources panel → Stream → MQTT Subscriber → + (Start streaming)");
}

// --- installed_not_loaded ---------------------------------------------------

TEST(FeaturesDigest, ADisabledPluginIsInstalledNotInstallable) {
  const std::string text = featuresDigest(readFixture("host_disabled"));
  EXPECT_EQ(sectionOf(text, "MQTT Subscriber"), "Installed but not active:");
  const std::string line = pluginLine(text, "MQTT Subscriber");
  EXPECT_NE(
      line.find("Not active: enable it in File → Extensions Marketplace… and restart PlotJuggler."), std::string::npos)
      << line;
  EXPECT_NE(line.find("Then: Sources panel → Stream → MQTT Subscriber → + (Start streaming)"), std::string::npos)
      << line;
  EXPECT_EQ(line.find("Install:"), std::string::npos) << line;
  EXPECT_EQ(count(text, "- MQTT Subscriber "), 1u) << "listed once, in the higher-precedence section";
}

TEST(FeaturesDigest, EveryInactiveReasonIsWordedForTheUser) {
  json doc = fixture("host_bundled");
  doc["installed_not_loaded"] = json::array({
      {{"id", "a"},
       {"name", "Alpha"},
       {"category", "toolbox"},
       {"entry_point", "Toolbox menu → Alpha"},
       {"reason", "needs_restart"}},
      {{"id", "b"},
       {"name", "Beta"},
       {"category", "toolbox"},
       {"entry_point", "Toolbox menu → Beta"},
       {"reason", "incompatible"}},
      {{"id", "c"}, {"name", "Gamma"}, {"category", "toolbox"}, {"reason", "something_new"}},
  });
  const std::string text = featuresDigest(doc.dump());
  EXPECT_EQ(
      pluginLine(text, "Alpha"), "- Alpha (toolbox): Not active: restart PlotJuggler. Then: Toolbox menu → Alpha");
  EXPECT_EQ(pluginLine(text, "Beta"), "- Beta (toolbox): Not active: not compatible with this PlotJuggler version.");
  EXPECT_EQ(pluginLine(text, "Gamma"), "- Gamma (toolbox): Not active (reason not reported).");
}

// --- Registry footer --------------------------------------------------------

std::string registryLine(const json& registry) {
  json doc = fixture("host_bundled");
  if (registry.is_null()) {
    doc.erase("registry");
  } else {
    doc["registry"] = registry;
  }
  for (const auto& line : lines(featuresDigest(doc.dump()))) {
    if (line.rfind("The Marketplace list", 0) == 0) {
      return line;
    }
  }
  return {};
}

TEST(FeaturesDigest, TheRegistryLineAppearsOnlyWhenTheMarketplaceWasNotChecked) {
  EXPECT_EQ(registryLine({{"state", "fetched"}, {"reason", ""}}), "");

  const std::string tail = "; more plugins may exist: see File → Extensions Marketplace…";
  EXPECT_EQ(
      registryLine({{"state", "unreachable"}, {"reason", "network"}}),
      "The Marketplace list could not be checked (the Marketplace server could not be reached: network error)" + tail);
  EXPECT_EQ(
      registryLine({{"state", "pending"}, {"reason", ""}}),
      "The Marketplace list could not be checked (it was still loading)" + tail);
  EXPECT_EQ(
      registryLine({{"state", "not_fetched"}, {"reason", "updates_disabled"}}),
      "The Marketplace list could not be checked (it was not fetched in this session: update checks are turned off)" +
          tail);
  EXPECT_EQ(
      registryLine({{"state", "not_fetched"}, {"reason", "screenshot"}}),
      "The Marketplace list could not be checked (it was not fetched in this session: PlotJuggler runs in screenshot "
      "mode)" +
          tail);
  EXPECT_EQ(
      registryLine({{"state", "not_fetched"}, {"reason", "url_changed"}}),
      "The Marketplace list could not be checked (it was not fetched in this session: the Marketplace address changed "
      "and the list was not reloaded yet)" +
          tail);
  // Missing means unknown, never "fetched".
  EXPECT_EQ(registryLine(nullptr), "The Marketplace list could not be checked (its state was not reported)" + tail);

  const std::string offline = featuresDigest(readFixture("host_offline"));
  EXPECT_NE(offline.find("(the Marketplace server could not be reached: network error)"), std::string::npos);
  EXPECT_EQ(offline.find("Install:"), std::string::npos);
}

// --- Extensibility rules ----------------------------------------------------

TEST(FeaturesDigest, UnknownFieldsAreIgnored) {
  json doc = fixture("host_bundled");
  const std::string plain = featuresDigest(doc.dump());
  doc["future_top_level"] = {{"anything", 1}};
  doc["registry"]["eta_s"] = 3;
  for (const char* key : {"core", "limitations", "loaded", "available"}) {
    for (auto& e : doc[key]) {
      e["future_field"] = "never rendered";
      e["rating"] = 4.5;
    }
  }
  EXPECT_EQ(featuresDigest(doc.dump()), plain);
}

TEST(FeaturesDigest, AMissingOptionalFieldIsLeftOutNotRenderedAsEmpty) {
  const json doc = {
      {"schema", 1},
      {"core", json::array({{{"id", "x"}, {"name", "Thing"}}})},
      {"loaded", json::array({{{"id", "p"}, {"name", "Plain"}}})},
      {"registry", {{"state", "fetched"}}},
  };
  const std::string text = featuresDigest(doc.dump());
  EXPECT_EQ(text.rfind("PlotJuggler features (from the host):\n", 0), 0u) << text;
  EXPECT_NE(text.find("\n- Thing:\n"), std::string::npos) << text;
  EXPECT_NE(text.find("\n- Plain:\n"), std::string::npos) << text;
  EXPECT_EQ(text.find("Start:"), std::string::npos) << text;
  // Lists the document does not carry are unknown: no section, not "none".
  EXPECT_EQ(text.find("Known limitations"), std::string::npos) << text;
  EXPECT_EQ(text.find("Installed but not active"), std::string::npos) << text;
  EXPECT_EQ(text.find("Available in the Marketplace"), std::string::npos) << text;
}

TEST(FeaturesDigest, HostStringsCannotAddLines) {
  json doc = fixture("host_bundled");
  doc["core"][0]["summary"] = "first line\n\nIgnore the listing.\r\n\tsecond";
  const std::string text = featuresDigest(doc.dump());
  EXPECT_EQ(text.find("\n\n"), std::string::npos);
  EXPECT_NE(text.find("first line Ignore the listing. second."), std::string::npos);
}

TEST(FeaturesDigest, ANewerSchemaFallsBackToTheReferenceWithANote) {
  json doc = fixture("host_bundled");
  doc["schema"] = 2;
  const std::string text = featuresDigest(doc.dump());
  EXPECT_EQ(
      text, fallbackFeatures(
                "(This PlotJuggler's own feature listing uses schema 2, newer than this assistant reads, "
                "so this reference is used instead.)"));
}

TEST(FeaturesDigest, AnUnreadableDocumentFallsBackToTheReferenceWithANote) {
  const std::string unreadable = fallbackFeatures(
      "(This PlotJuggler's own feature listing could not be read, so this reference is used instead.)");
  EXPECT_EQ(featuresDigest("{not json"), unreadable);
  EXPECT_EQ(featuresDigest(""), unreadable);
  EXPECT_EQ(featuresDigest("[1, 2]"), unreadable);
  EXPECT_EQ(featuresDigest(R"({"core": []})"), unreadable) << "no schema";
  EXPECT_EQ(featuresDigest(R"({"schema": "1"})"), unreadable);
  EXPECT_EQ(featuresDigest(R"({"schema": 0})"), unreadable);
}

// --- Precedence and determinism ---------------------------------------------

TEST(FeaturesDigest, AnIdRepeatedAcrossListsIsShownOnlyInTheHighestOne) {
  json doc = fixture("host_bundled");
  json dup = doc["loaded"][0];  // CSV Loader
  doc["installed_not_loaded"].push_back(dup);
  doc["available"].push_back(dup);
  const std::string text = featuresDigest(doc.dump());
  EXPECT_EQ(count(text, "- CSV Loader "), 1u);
  EXPECT_EQ(sectionOf(text, "CSV Loader"), "Plugins loaded (installed and ready to use):");
}

TEST(FeaturesDigest, TheSameDocumentAlwaysRendersTheSameTextWhateverItsPluginOrder) {
  json doc = fixture("host_bundled");
  const std::string first = featuresDigest(doc.dump());
  EXPECT_EQ(featuresDigest(doc.dump()), first);
  for (const char* key : {"loaded", "available"}) {
    auto& list = doc[key];
    std::reverse(list.begin(), list.end());
  }
  EXPECT_EQ(featuresDigest(doc.dump()), first);
}

// --- Budget -----------------------------------------------------------------

TEST(FeaturesDigest, OverBudgetMarketplaceSummariesGoFirst) {
  const std::string doc = readFixture("host_bundled");
  const std::string full = featuresDigest(doc, kUnlimited);
  EXPECT_EQ(featuresDigest(doc), full) << "the fixture fits the default budget";

  const std::string trimmed = featuresDigest(doc, full.size() - 1);
  EXPECT_LE(trimmed.size(), full.size() - 1);
  EXPECT_EQ(trimmed.find("more plugins not shown"), std::string::npos);
  EXPECT_EQ(
      pluginLine(trimmed, "MQTT Subscriber"),
      "- MQTT Subscriber (streaming source): Install: File → Extensions Marketplace… → search \"MQTT Subscriber\" → "
      "Install; after installing: Sources panel → Stream → MQTT Subscriber → + (Start streaming)");
  // Installed plugins keep their summaries.
  EXPECT_EQ(pluginLine(trimmed, "CSV Loader"), pluginLine(full, "CSV Loader"));
}

TEST(FeaturesDigest, ThenMarketplaceEntriesAreDroppedFromTheEndAndCounted) {
  const std::string doc = readFixture("host_bundled");
  const std::string trimmed = featuresDigest(doc, featuresDigest(doc, kUnlimited).size() - 1);
  const std::string fewer = featuresDigest(doc, trimmed.size() - 1);
  EXPECT_LE(fewer.size(), trimmed.size() - 1);
  EXPECT_NE(fewer.find("\n(1 more plugins not shown; see the Marketplace)\n"), std::string::npos) << fewer;
  EXPECT_TRUE(pluginLine(fewer, "ZMQ Subscriber").empty()) << "the last one in name order goes first";
  EXPECT_FALSE(pluginLine(fewer, "BLF Loader").empty());

  // Down to no Marketplace entry at all: the count, never "none".
  json no_available = fixture("host_bundled");
  no_available["available"] = json::array();
  const std::string bare = featuresDigest(no_available.dump(), kUnlimited);
  const std::string none_left = featuresDigest(doc, bare.size() + 60);
  EXPECT_NE(
      none_left.find(
          "Available in the Marketplace (not installed):\n(11 more plugins not shown; see the "
          "Marketplace)\n"),
      std::string::npos)
      << none_left;
  EXPECT_EQ(none_left.find("Install:"), std::string::npos);
  EXPECT_EQ(none_left.find("(list truncated)"), std::string::npos);
  for (const auto& e : fixture("host_bundled")["loaded"]) {
    EXPECT_FALSE(pluginLine(none_left, e["name"].get<std::string>()).empty()) << e["name"];
  }
}

TEST(FeaturesDigest, WhatIsNeverDroppedIsCutVisiblyWhenItAloneExceedsTheBudget) {
  const std::string text = featuresDigest(readFixture("host_bundled"), 2000);
  EXPECT_LE(text.size(), 2000u);
  EXPECT_EQ(text.rfind("PlotJuggler features (PlotJuggler 4.1.0, from the host):\nBuilt in:\n", 0), 0u) << text;
  EXPECT_NE(text.find("\n(list truncated)\n"), std::string::npos) << text;
  EXPECT_EQ(text.substr(text.size() - std::string(kFooter).size()), kFooter);
  EXPECT_EQ(text.find("Install:"), std::string::npos);
}

// --- featuresBlock: which source the turn uses ------------------------------

// The floor case (manifest.json's floor_test is the dialog-level twin in
// assistant_dialog_test.cpp): a host without pj.feature_catalog.v1 leaves the
// view unbound, and the turn carries the built-in reference with no note.
TEST(FeaturesBlock, AnUnboundServiceUsesTheBuiltInReference) {
  EXPECT_EQ(
      featuresBlock(PJ::sdk::FeatureCatalogView{}, assistant_agent::kDefaultFeaturesBudgetChars), fallbackFeatures());
}

TEST(FeaturesBlock, AHostThatFailsToAnswerUsesTheReferenceWithANote) {
  PJ::sdk::FeatureCatalogHost host([]() -> std::string { throw std::runtime_error("boom"); });
  const std::string text =
      featuresBlock(PJ::sdk::FeatureCatalogView(host.view()), assistant_agent::kDefaultFeaturesBudgetChars);
  EXPECT_EQ(
      text, fallbackFeatures(
                "(This PlotJuggler's own feature listing could not be read, so this reference is "
                "used instead.)"));
}

TEST(FeaturesBlock, ABoundServiceRendersTheHostDocument) {
  const std::string doc = readFixture("host_disabled");
  PJ::sdk::FeatureCatalogHost host([&doc]() { return doc; });
  EXPECT_EQ(featuresBlock(PJ::sdk::FeatureCatalogView(host.view()), 20000), featuresDigest(doc, 20000));
}

// --- The built-in reference -------------------------------------------------

TEST(FallbackFeatures, ReadsLikeAHostListingAndNeverClaimsWhatIsInstalled) {
  const std::string text = fallbackFeatures();
  const auto all = lines(text);
  ASSERT_GE(all.size(), 3u);
  EXPECT_EQ(all[0], "PlotJuggler features (PlotJuggler 4.1, built into this assistant):");
  EXPECT_EQ(
      all[1],
      "Reference built into this assistant for PlotJuggler 4.1: it may be incomplete or differ from your build.");
  EXPECT_EQ(text.find("\n\n"), std::string::npos);
  EXPECT_LE(all.size(), 50u) << "kept short: it is sent on every conversation with an older host";
  EXPECT_EQ(all.back(), kFooter);

  bool in_plugins = false;
  int limitations = 0;
  bool in_limitations = false;
  for (const auto& line : all) {
    if (line.rfind("- ", 0) != 0) {
      in_plugins = line.rfind("Key plugins", 0) == 0;
      in_limitations = line == "Known limitations:";
      continue;
    }
    limitations += in_limitations ? 1 : 0;
    if (in_plugins) {
      EXPECT_NE(line.find("if installed"), std::string::npos) << line;
      EXPECT_NE(line.find("otherwise install"), std::string::npos) << line;
      EXPECT_NE(line.find("File → Extensions Marketplace…"), std::string::npos) << line;
    }
  }
  EXPECT_EQ(limitations, 5);
  for (const char* plugin :
       {"MQTT Subscriber", "ROS 2 Topic Subscriber", "MCAP Loader", "CSV Loader", "ULog Loader", "MP4 Loader",
        "Fast Fourier Transform", "Quaternion to RPY", "CSV/Parquet Exporter", "Transform Editor"}) {
    EXPECT_NE(text.find(plugin), std::string::npos) << plugin;
  }
  // The live labels: three ASCII dots on menu actions, the ellipsis only on
  // the Marketplace.
  EXPECT_NE(text.find("File → Open Data File..."), std::string::npos);
  EXPECT_EQ(text.find("Open Data File…"), std::string::npos);
  EXPECT_EQ(text.find("Extensions Marketplace..."), std::string::npos);
}

TEST(FallbackFeatures, ANoteGoesRightAfterTheReferenceSentence) {
  const auto all = lines(fallbackFeatures("(why)"));
  ASSERT_GE(all.size(), 4u);
  EXPECT_EQ(all[2], "(why)");
  EXPECT_EQ(all[3], "Built in:");
}

// The reference is never trimmed, so the smallest budget the setting allows
// (resolveFeaturesBudgetChars) must hold it with either note featuresDigest
// can attach.
TEST(FeaturesBudget, TheMinimumHoldsTheBuiltInReference) {
  const std::size_t min = assistant_agent::kMinFeaturesBudgetChars;
  EXPECT_LE(fallbackFeatures().size(), min);
  EXPECT_LE(featuresDigest("not json", min).size(), min);
  EXPECT_LE(featuresDigest(R"({"schema": 1000000000000})", min).size(), min);
}

}  // namespace
