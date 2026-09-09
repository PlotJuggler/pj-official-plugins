// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Drives the ToolRegistry executors against fake C-ABI hosts: a ToolboxTestStore
// for the read tools (list/describe/read) and a hand-rolled recording
// data-processors host for the write tools (create_derived_series /
// create_markers), asserting the exact script + inputs the tool synthesizes.
#include "tool_registry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>
#include <tuple>
#include <vector>

#include "support/fake_catalog_host.hpp"
#include "support/fake_multi_dataset_host.hpp"
#include "support/fake_object_read_host.hpp"
#include "support/fake_playback_viewport_hosts.hpp"
#include "support/fake_plot_tabs_host.hpp"
#include "support/recording_dp_host.hpp"

namespace {

using assistant_agent::catalogDigest;
using assistant_agent::ToolContext;
using assistant_agent::ToolRegistry;
using assistant_agent::testing::FakeCatalogHost;
using assistant_agent::testing::FakeMultiDatasetHost;
using assistant_agent::testing::FakeObjectReadHost;
using assistant_agent::testing::FakePlaybackHost;
using assistant_agent::testing::FakePlotTabsHost;
using assistant_agent::testing::FakeViewportHost;
using assistant_agent::testing::RecordingDpHost;
using nlohmann::json;

constexpr std::int64_t kSec = 1'000'000'000;

// Populate a small catalog: /imu with fields x (0..4) and y (constant 2).
// ToolboxTestStore is non-movable (it hands out pointers into itself), so
// callers construct it locally and pass it here by reference.
void populate(PJ::testing::ToolboxTestStore& store) {
  store.addTopic("/imu");
  store.addField("/imu", "x", {0, kSec, 2 * kSec, 3 * kSec, 4 * kSec}, {0.0, 1.0, 2.0, 3.0, 4.0});
  store.addField("/imu", "y", {0, kSec, 2 * kSec}, {2.0, 2.0, 2.0});
}

ToolContext makeCtx(PJ::testing::ToolboxTestStore& store, RecordingDpHost* dp, FakeObjectReadHost* objects = nullptr) {
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(store.makeHost());
  if (dp != nullptr) {
    ctx.dp = dp->view();
  }
  if (objects != nullptr) {
    ctx.objects = PJ::sdk::ToolboxObjectReadHostView(objects->makeHost());
  }
  return ctx;
}

TEST(ToolRegistry, ListsAllToolsAndSchemas) {
  ToolRegistry reg;
  EXPECT_EQ(reg.tools().size(), 12u);
  // Both serializations expose every tool by name.
  EXPECT_EQ(reg.toFunctionSpecs().size(), 12u);
  EXPECT_EQ(reg.toMcpToolsList().size(), 12u);
  EXPECT_NE(reg.find("evaluate"), nullptr);
  EXPECT_NE(reg.find("create_derived_series"), nullptr);
  EXPECT_NE(reg.find("playback"), nullptr);
  EXPECT_EQ(reg.find("play"), nullptr);
  EXPECT_NE(reg.find("plot_tab"), nullptr);
  // zoom_to_time_range/zoom_reset were folded into plot_tab's 'zoom' action:
  // the user's plots are no longer reachable from any tool, only the tabs
  // this assistant composed itself.
  EXPECT_EQ(reg.find("zoom_to_time_range"), nullptr);
  EXPECT_EQ(reg.find("zoom_reset"), nullptr);
  // remove_markers exists but is scoped to the assistant's own marker set;
  // no tool can touch user data destructively.
  EXPECT_NE(reg.find("remove_markers"), nullptr);
  EXPECT_EQ(reg.find("delete_everything"), nullptr);
}

TEST(ToolRegistry, UnknownToolIsCleanFailure) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("nope", json::object(), ctx);
  EXPECT_FALSE(r.ok);
}

TEST(ToolRegistry, ListTopics) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("list_topics", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["topics"][0]["topic"], "/imu");
  EXPECT_EQ(j["topics"][0]["fields"], 2);
}

TEST(ToolRegistry, DescribeTopicListsFieldPaths) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("describe_topic", {{"topic", "/imu"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  ASSERT_EQ(j["fields"].size(), 2u);
  EXPECT_EQ(j["fields"][0]["path"], "/imu/x");
}

TEST(ToolRegistry, DescribeUnknownTopicFails) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("describe_topic", {{"topic", "/nope"}}, ctx);
  EXPECT_FALSE(r.ok);
}

TEST(ToolRegistry, ReadSeriesStats) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["stats"]["count"], 5);
  EXPECT_DOUBLE_EQ(j["stats"]["min"].get<double>(), 0.0);
  EXPECT_DOUBLE_EQ(j["stats"]["max"].get<double>(), 4.0);
  // The spacing facts ride along in the same report, self-described by key.
  EXPECT_TRUE(j["stats"].contains("max_gap_s"));
  EXPECT_TRUE(j["stats"].contains("max_gap_at_s"));
}

// A NaN sample must not silently poison min/max/mean/stddev: the tool reports
// it as 'invalid'/'invalid_fraction' instead, and only when it is present.
TEST(ToolRegistry, ReadSeriesStatsFlagsNonFiniteValues) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  store.addField("/imu", "nan_x", {0, kSec, 2 * kSec, 3 * kSec}, {1.0, std::nan(""), 3.0, 4.0});
  auto ctx = makeCtx(store, nullptr);

  auto dirty = reg.execute("read_series", {{"series", "/imu/nan_x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(dirty.ok) << dirty.content;
  auto dj = json::parse(dirty.content);
  EXPECT_EQ(dj["stats"]["invalid"], 1);
  EXPECT_DOUBLE_EQ(dj["stats"]["invalid_fraction"].get<double>(), 0.25);
  EXPECT_DOUBLE_EQ(dj["stats"]["min"].get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(dj["stats"]["max"].get<double>(), 4.0);

  auto clean = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(clean.ok) << clean.content;
  auto cj = json::parse(clean.content);
  EXPECT_FALSE(cj["stats"].contains("invalid"));
  EXPECT_FALSE(cj["stats"].contains("invalid_fraction"));
}

TEST(ToolRegistry, ReadSeriesBuckets) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "buckets"}, {"max_points", 3}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_TRUE(j.contains("buckets"));
  EXPECT_LE(j["buckets"].size(), 5u);
}

TEST(ToolRegistry, ReadUnknownSeriesFails) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/z"}}, ctx);
  EXPECT_FALSE(r.ok);
}

// A missing series must point the model at the one check that would settle it,
// instead of letting it conclude the signal does not exist from a stale listing.
TEST(ToolRegistry, UnknownSeriesErrorTellsTheModelToVerifyOnce) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/nope"}}, ctx);
  ASSERT_FALSE(r.ok);
  EXPECT_NE(r.content.find("list_topics"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("once"), std::string::npos) << r.content;
}

// An abbreviated path resolves when only one series can match — this is what
// keeps a slightly-wrong path from costing a correction round-trip.
TEST(ToolRegistry, AbbreviatedSeriesPathResolvesWhenUnambiguous) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);  // /imu with fields x and y
  auto ctx = makeCtx(store, nullptr);
  // "x" alone names exactly one series (/imu/x), so it must resolve.
  auto r = reg.execute("read_series", {{"series", "x"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["stats"]["count"], 5);
}

// ...but never by guessing: two candidates must come back as a listed choice.
TEST(ToolRegistry, AmbiguousSeriesPathListsCandidatesInsteadOfGuessing) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/left");
  store.addField("/left", "speed", {0, kSec}, {1.0, 2.0});
  store.addTopic("/right");
  store.addField("/right", "speed", {0, kSec}, {3.0, 4.0});
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "speed"}}, ctx);
  ASSERT_FALSE(r.ok) << "picking one of two same-named signals silently is worse than failing";
  EXPECT_NE(r.content.find("/left/speed"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("/right/speed"), std::string::npos) << r.content;
}

// Whole-segment matching: half a segment must NOT resolve, or "/im" would
// silently attach to "/imu/x".
TEST(ToolRegistry, PartialSegmentDoesNotResolve) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/im"}}, ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_NE(r.content.find("unknown series"), std::string::npos) << r.content;
}

// Inputs are resolved before the transform is installed. Without this a
// mistyped input installs happily and yields an empty curve — a failure that
// looks like success, which is the worst outcome available here.
TEST(ToolRegistry, CreateDerivedSeriesRejectsAnInputThatNamesNothing) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series",
      {{"name", "bogus"}, {"inputs", json::array({"/imu/does_not_exist"})}, {"expression", "value * 2"}}, ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_TRUE(dp.last_inputs.empty()) << "nothing should have been installed";
}

// And an abbreviated input is expanded to the real path on the way through, so
// the host receives what it expects.
TEST(ToolRegistry, CreateDerivedSeriesExpandsAnAbbreviatedInput) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "doubled"}, {"inputs", json::array({"x"})}, {"expression", "value * 2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  ASSERT_EQ(dp.last_inputs.size(), 1u);
  EXPECT_EQ(dp.last_inputs[0], "/imu/x");
}

// list_topics is re-sent on every later round-trip of a turn, so its payload
// has to stay bounded however large a limit the model asks for.
TEST(ToolRegistry, ListTopicsCapsAnAbsurdLimit) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  for (int i = 0; i < 600; ++i) {
    const std::string name = "/t" + std::to_string(i);
    store.addTopic(name);
    store.addField(name, "value", {0}, {1.0});
  }
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("list_topics", {{"limit", 100000}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 600);
  EXPECT_LE(j["shown"].get<int>(), 500);
  EXPECT_TRUE(j.contains("note")) << "a truncated listing must say so";
}

// The digest is what replaces the discovery round-trips, so it has to carry the
// full paths...
TEST(CatalogDigest, ListsTopicsAndFields) {
  PJ::testing::ToolboxTestStore store;
  populate(store);
  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(store.makeHost()));
  EXPECT_NE(digest.find("/imu"), std::string::npos) << digest;
  EXPECT_NE(digest.find("x"), std::string::npos) << digest;
}

// ...and, when it cannot carry everything, say so loudly. A model that trusts a
// silently-truncated listing will tell the user a signal does not exist.
TEST(CatalogDigest, SaysSoWhenTruncated) {
  PJ::testing::ToolboxTestStore store;
  for (int i = 0; i < 400; ++i) {
    const std::string name = "/topic_with_a_fairly_long_name_" + std::to_string(i);
    store.addTopic(name);
    store.addField(name, "value", {0}, {1.0});
  }
  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(store.makeHost()), /*budget_chars=*/1000);
  EXPECT_LT(digest.size(), 2000u) << "the digest must respect its budget";
  EXPECT_NE(digest.find("TRUNCATED"), std::string::npos) << digest;
  EXPECT_NE(digest.find("list_topics"), std::string::npos) << digest;
}

// The measured problem this tiering exists for: a catalog where a handful of
// topics are so field-heavy that giving every topic its full field list blows
// the budget for ALL of them — the old algorithm's only move — even though
// most topics are small enough to afford it easily. The mixed digest keeps
// every topic's full fields when cheap and only trims the expensive ones.
TEST(CatalogDigest, MixesFullPartialAndCounts) {
  FakeCatalogHost host;
  for (int i = 0; i < 30; ++i) {
    const std::string topic = "/small_" + (i < 10 ? std::string("0") : std::string()) + std::to_string(i);
    host.addTopic(topic);
    host.addField(topic, "s" + std::to_string(i) + "_alpha");
    host.addField(topic, "s" + std::to_string(i) + "_beta");
    host.addField(topic, "s" + std::to_string(i) + "_gamma");
  }
  for (int t = 0; t < 2; ++t) {
    const std::string topic = "/fat_" + std::to_string(t);
    host.addTopic(topic);
    for (int f = 0; f < 60; ++f) {
      std::string name = "channel_" + std::to_string(t) + "_";
      if (f < 10) {
        name += "0";
      }
      name += std::to_string(f);
      host.addField(topic, name);
    }
  }

  // Tight enough that the two 60-field topics cannot both afford a full
  // listing (each would cost ~800+ characters) while the 30 three-field
  // topics can (each costs only ~20) -- the budget the real ALFA catalog
  // exercises this same way, just smaller.
  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()), /*budget_chars=*/1800);

  EXPECT_EQ(digest.find("TRUNCATED"), std::string::npos) << "the mixed tiering must never need to drop a topic "
                                                            "entirely -- every topic gets at least a count: "
                                                         << digest;
  // Every small topic keeps its full field list.
  for (int i = 0; i < 30; ++i) {
    const std::string field = "s" + std::to_string(i) + "_gamma";
    EXPECT_NE(digest.find(field), std::string::npos) << "small topic " << i << " lost its fields: " << digest;
  }
  // Neither fat topic's full field list survives -- its last field is never
  // reached by a count or a 120-char partial list.
  EXPECT_EQ(digest.find("channel_0_59"), std::string::npos) << digest;
  EXPECT_EQ(digest.find("channel_1_59"), std::string::npos) << digest;
  EXPECT_NE(digest.find("describe_topic gives the rest"), std::string::npos) << digest;
}

// A field type shared by 80%+ of the catalog is stated once and left off
// every field that has it; a minority type still gets its own annotation, so
// the reader can tell the exception from the rule.
TEST(CatalogDigest, OmitsTheDominantType) {
  FakeCatalogHost host;
  host.addTopic("/mostly_int");
  for (int i = 0; i < 9; ++i) {
    host.addField("/mostly_int", "n" + std::to_string(i), PJ_PRIMITIVE_TYPE_INT32);
  }
  host.addField("/mostly_int", "odd_one_out", PJ_PRIMITIVE_TYPE_FLOAT64);

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()));

  EXPECT_NE(digest.find("fields are int32 unless marked"), std::string::npos) << digest;
  EXPECT_EQ(digest.find("n0 (int32)"), std::string::npos) << "the dominant type must be left off: " << digest;
  EXPECT_NE(digest.find("n0"), std::string::npos) << digest;
  EXPECT_NE(digest.find("odd_one_out (float64)"), std::string::npos)
      << "the minority type must still be stated: " << digest;
}

// When the budget cannot afford full field lists for every topic that would
// like one, the cheapest upgrades go first -- and among ties, catalog order
// decides, not map/hash order, so a re-run never reshuffles who won.
TEST(CatalogDigest, PromotesCheapestFirstDeterministically) {
  FakeCatalogHost host;
  host.addTopic("/imu");
  host.addField("/imu", "roll");
  host.addField("/imu", "pitch");
  host.addField("/imu", "yaw");
  // A much larger topic so the budget is tight enough that not everything
  // upgrades to full, without being so tight that /imu itself is squeezed.
  host.addTopic("/big");
  for (int f = 0; f < 40; ++f) {
    std::string name = "reading_";
    if (f < 10) {
      name += "0";
    }
    name += std::to_string(f);
    host.addField("/big", name);
  }

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()), /*budget_chars=*/400);

  // /imu is small and comes first in the catalog: its full, tied-cost fields
  // win the budget deterministically over /big's.
  EXPECT_NE(digest.find("roll"), std::string::npos) << digest;
  EXPECT_NE(digest.find("pitch"), std::string::npos) << digest;
  EXPECT_NE(digest.find("yaw"), std::string::npos) << digest;
  EXPECT_EQ(digest.find("reading_039"), std::string::npos) << "/big must not have won a full listing: " << digest;
}

// The floor beneath the mixed tiering: a budget so tight that not even a bare
// field COUNT fits for every topic falls back to the pre-existing plain
// truncation (full tree, then names only), unchanged.
TEST(CatalogDigest, FallsBackToPlainTruncationWhenNotEvenCountsFit) {
  FakeCatalogHost host;
  for (int i = 0; i < 400; ++i) {
    const std::string name = "/topic_with_a_fairly_long_name_" + std::to_string(i);
    host.addTopic(name);
    host.addField(name, "value");
  }

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()), /*budget_chars=*/1000);

  EXPECT_LT(digest.size(), 2000u) << "the digest must respect its budget";
  EXPECT_NE(digest.find("TRUNCATED"), std::string::npos) << digest;
  EXPECT_NE(digest.find("list_topics"), std::string::npos) << digest;
  EXPECT_EQ(digest.find("describe_topic gives the rest"), std::string::npos)
      << "this is the legacy truncation footer, not the mixed-tier one: " << digest;
}

TEST(CatalogDigest, SaysWhenNothingIsLoaded) {
  PJ::testing::ToolboxTestStore store;
  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(store.makeHost()));
  EXPECT_NE(digest.find("nothing is loaded"), std::string::npos) << digest;
}

TEST(ToolRegistry, CreateDerivedSeriesFromExpression) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.validate_calls, 1);
  EXPECT_EQ(dp.create_calls, 1);
  EXPECT_EQ(dp.last_kind, "transform");
  EXPECT_EQ(dp.last_inputs, (std::vector<std::string>{"/imu/x"}));
  EXPECT_EQ(dp.last_outputs, (std::vector<std::string>{"x2"}));
  EXPECT_NE(dp.last_script.find("return (value * 2)"), std::string::npos);
}

TEST(ToolRegistry, CreateDerivedSeriesStatefulBody) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series",
      {{"name", "deriv"},
       {"inputs", json::array({"/imu/x"})},
       {"global", "local pt, pv"},
       {"body", "return value - (pv or value)"}},
      ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_NE(dp.last_script.find("local pt, pv"), std::string::npos);
  EXPECT_NE(dp.last_script.find("return value - (pv or value)"), std::string::npos);
}

TEST(ToolRegistry, CreateDerivedSeriesSurfacesValidateError) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_validate = true;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "bad"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value +"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 0);  // never installed on a compile failure
}

// The runtime cross-read: a single declared input, with the OTHER series read
// inside the body via the marker-rule vocabulary. The empty-join guard only
// inspects declared inputs, so this sidesteps it — and the real host then
// refuses the install, because `series(...)` does not exist in a transform's
// environment (verified in the application, 2026-08-14). The fake host mirrors
// that refusal; this test pins that the refusal reaches the model as a plain
// failure, so the benchmark measures the reaction the product would produce.
TEST(ToolRegistry, CreateDerivedSeriesRuntimeCrossReadIsRefusedLikeTheRealHost) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series",
      {{"name", "mix"},
       {"inputs", json::array({"/imu/x"})},
       {"body", "local other = series(\"/imu/y\"):atTime(time)\nreturn value + other"}},
      ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("series"), std::string::npos) << r.content;
  EXPECT_EQ(dp.liveCount(), 0) << "nothing may be left installed after the refusal";
}

TEST(ToolRegistry, CreateDerivedSeriesUnboundDpDegrades) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);  // no data-processors host
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("data_processors"), std::string::npos);
}

TEST(ToolRegistry, CreateMarkersBuildsThresholdRule) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_kind, "markers");
  // Default style is "region": one shaded band per exceedance stretch.
  EXPECT_NE(dp.last_script.find("startMarker"), std::string::npos);
  EXPECT_NE(dp.last_script.find("closeMarker"), std::string::npos);
  EXPECT_NE(dp.last_script.find("> 2.5"), std::string::npos);

  // style="line" keeps the per-sample vertical markers.
  r = reg.execute(
      "create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}, {"style", "line"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_NE(dp.last_script.find("createVerticalMarker"), std::string::npos);
}

TEST(ToolRegistry, CreateMarkersRawRulePassesThroughVerbatim) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  const std::string rule = "local s = series(\"/imu/x\")\nstartMarker(0)\ncloseMarker(100)\n";
  auto r = reg.execute("create_markers", {{"inputs", json::array({"/imu/x"})}, {"rule", rule}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_kind, "markers");
  EXPECT_EQ(dp.last_script, rule);  // authored by the model, never rewritten
  ASSERT_EQ(dp.last_inputs.size(), 1u);
  EXPECT_EQ(dp.last_inputs[0], "/imu/x");
  // Default output marker topic = first input.
  ASSERT_EQ(dp.last_outputs.size(), 1u);
  EXPECT_EQ(dp.last_outputs[0], "/imu/x");
}

TEST(ToolRegistry, CreateMarkersRejectsMixedForms) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  // rule + template fields together is ambiguous -> clean failure, no create.
  auto r = reg.execute(
      "create_markers", {{"rule", "startMarker(0)"}, {"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}},
      ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 0);
  // rule without inputs -> clean failure.
  r = reg.execute("create_markers", {{"rule", "startMarker(0)"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 0);
}

TEST(ToolRegistry, RemoveMarkersRemovesOwnSet) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute("remove_markers", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_removed, "assistant_markers");
}

TEST(ToolRegistry, CreateMarkersDegradesOnHostRejection) {
  // Mirrors PJ4 main, where the host rejects kind="markers": the tool surfaces
  // the host error to the model rather than crashing.
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_create = true;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("rejected"), std::string::npos);
}

TEST(ToolRegistry, ReportStatus) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("report_status", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["topics"], 1);
  EXPECT_EQ(j["fields"], 2);
}

// --- what the model is told about what it just made ------------------------

namespace {
// A marker set as the host would publish it: `regions` shaded spans followed by
// `events` per-sample ticks.
PJ::sdk::PlotMarkers makeSet(std::size_t regions, std::size_t events) {
  PJ::sdk::PlotMarkers set;
  for (std::size_t i = 0; i < regions; ++i) {
    PJ::sdk::PlotMarker m;
    m.kind = PJ::sdk::MarkerKind::kRegion;
    m.t_start = static_cast<PJ::Timestamp>(i) * kSec;
    m.t_end = m.t_start + kSec / 2;
    set.markers.push_back(m);
  }
  for (std::size_t i = 0; i < events; ++i) {
    PJ::sdk::PlotMarker m;
    m.kind = PJ::sdk::MarkerKind::kEvent;
    m.t_start = static_cast<PJ::Timestamp>(i) * (kSec / 100);
    set.markers.push_back(m);
  }
  return set;
}
}  // namespace

// The whole point of the read-back: from the model's seat, twelve tidy regions
// and four thousand overlapping lines are the same "created" message. The
// breakdown by kind is what separates them.
TEST(ToolRegistry, CreateMarkersReportsWhatWasActuallyPublished) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  FakeObjectReadHost objects;
  objects.publish("/imu/x", makeSet(/*regions=*/3, /*events=*/0));
  ToolContext ctx = makeCtx(store, &dp, &objects);

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["markers_created"], 3);
  EXPECT_EQ(j["by_kind"]["regions"], 3);
  EXPECT_FALSE(j["by_kind"].contains("events"));
  // Three half-second regions cover 1.5 s; the envelope they sit in is 2.5 s.
  EXPECT_DOUBLE_EQ(j["covered_s"].get<double>(), 1.5);
  EXPECT_FALSE(j.contains("span_s"));
}

// The field is named `covered_s` and it has to earn the name: overlapping
// regions merge instead of double-counting, and the stretch BETWEEN regions is
// not covered. The envelope of this set is 11 s and the naive duration sum is
// 5 s; only the union, 4 s, is what a user would call "time covered". Its
// predecessor `span_s` reported the envelope, and models quoted it as coverage
// in every session that created markers.
TEST(ToolRegistry, CoveredTimeIsTheUnionOfRegionsNotTheEnvelope) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  FakeObjectReadHost objects;
  PJ::sdk::PlotMarkers set;
  for (const auto& [start_s, end_s] : {std::pair{0, 2}, {1, 3}, {10, 11}}) {
    PJ::sdk::PlotMarker m;
    m.kind = PJ::sdk::MarkerKind::kRegion;
    m.t_start = static_cast<PJ::Timestamp>(start_s) * kSec;
    m.t_end = static_cast<PJ::Timestamp>(end_s) * kSec;
    set.markers.push_back(m);
  }
  objects.publish("/imu/x", set);
  ToolContext ctx = makeCtx(store, &dp, &objects);

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_DOUBLE_EQ(j["covered_s"].get<double>(), 4.0);
}

TEST(ToolRegistry, CreateMarkersMakesAWallVisibleAsSuch) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  FakeObjectReadHost objects;
  objects.publish("/imu/x", makeSet(/*regions=*/0, /*events=*/4182));
  ToolContext ctx = makeCtx(store, &dp, &objects);

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["markers_created"], 4182);
  EXPECT_EQ(j["by_kind"]["events"], 4182);
  // Zero-duration marks cover nothing; an absent field beats a meaningless 0.
  EXPECT_FALSE(j.contains("covered_s"));
}

// entry_count() reports 1 for any set, because MarkerService pushes the whole
// blob as a single entry. If the production code ever regresses to trusting it,
// this is the test that catches it: the count would read 1 instead of 3.
TEST(ToolRegistry, MarkerCountComesFromThePayloadNotTheEntryCount) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  FakeObjectReadHost objects;
  objects.publish("/imu/x", makeSet(/*regions=*/3, /*events=*/0));
  ToolContext ctx = makeCtx(store, &dp, &objects);

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["markers_created"], 3) << "1 here means entry_count() was trusted";
}

// The read service is optional. Without it the answer simply carries no count —
// creating markers must not start failing on a host that omits it.
TEST(ToolRegistry, CreateMarkersSucceedsWithoutTheObjectReadService) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);  // no object host

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["created_markers_on"], "/imu/x");
  EXPECT_FALSE(j.contains("markers_created"));
}

// --- refusing to build an empty curve --------------------------------------

// Two inputs that exist, are numeric, and share not one timestamp: the host
// joins multi-input transforms on exact timestamp equality, so this would
// install a series with zero points and report success. Reproduced from the
// real shape of the problem — same rate, offset by half a sample — because a
// rate comparison would call these compatible and be wrong.
TEST(ToolRegistry, RefusesATransformWhoseInputsShareNoTimestamps) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/a");
  store.addField("/a", "v", {0, kSec, 2 * kSec}, {1.0, 2.0, 3.0});
  store.addTopic("/b");
  store.addField("/b", "v", {kSec / 2, 3 * kSec / 2, 5 * kSec / 2}, {1.0, 2.0, 3.0});
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series",
      {{"name", "mix"}, {"inputs", json::array({"/a/v", "/b/v"})}, {"expression", "value + v1"}}, ctx);

  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.persistent_creates, 0) << "nothing may be installed";
  // The message has to carry both halves: why it refused, and what does work.
  EXPECT_NE(r.content.find("share no timestamps"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("read_series"), std::string::npos) << r.content;
}

// The other half of the contract, and the one that keeps this from becoming an
// over-restrictive guard: inputs on a common clock must still work.
TEST(ToolRegistry, AllowsATransformWhoseInputsShareATimeline) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/a");
  store.addField("/a", "v", {0, kSec, 2 * kSec}, {1.0, 2.0, 3.0});
  store.addTopic("/b");
  store.addField("/b", "v", {0, kSec, 2 * kSec}, {4.0, 5.0, 6.0});
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series",
      {{"name", "mix"}, {"inputs", json::array({"/a/v", "/b/v"})}, {"expression", "value + v1"}}, ctx);

  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.persistent_creates, 1);
  // Fully overlapping, so no partial-join warning is emitted.
  EXPECT_FALSE(json::parse(r.content).contains("joined_points"));
}

// A partial overlap is legal and gets built — but silently dropping most of the
// rows is exactly the kind of surprise the model cannot see, so it is named.
TEST(ToolRegistry, ReportsHowManyPointsSurviveAPartialJoin) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/a");
  store.addField("/a", "v", {0, kSec, 2 * kSec, 3 * kSec}, {1.0, 2.0, 3.0, 4.0});
  store.addTopic("/b");
  store.addField("/b", "v", {0, 5 * kSec, 6 * kSec, 7 * kSec}, {4.0, 5.0, 6.0, 7.0});
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series",
      {{"name", "mix"}, {"inputs", json::array({"/a/v", "/b/v"})}, {"expression", "value + v1"}}, ctx);

  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["points"], 1) << "only t=0 is common";
  EXPECT_EQ(j["shortest_input_points"], 4);
  // The result states the outcome and stops there. It used to append
  // "verify_with: read_series on ...", and models obliged — a full round trip
  // spent fetching a number this response already had. What to do about the
  // result is the model's call; this tool's job is to report it.
  EXPECT_FALSE(j.contains("verify_with")) << "a tool result must not prescribe the next call";
}

// Single-input transforms have nothing to join, so the guard must not touch
// them — this is the overwhelmingly common case.
TEST(ToolRegistry, SingleInputTransformIsNeverForecast) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series", {{"name", "doubled"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}},
      ctx);

  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.persistent_creates, 1);
  const json j = json::parse(r.content);
  EXPECT_FALSE(j.contains("shortest_input_points")) << "nothing joined, so nothing was lost to a join";
  // Length still gets reported, because it is the fact the model would otherwise
  // spend a round trip to fetch. With one input there is no join to shorten it,
  // so the output is exactly as long as the input.
  EXPECT_EQ(j["points"], 5) << "as long as its only input, which populate() gives 5 samples";
  EXPECT_FALSE(j.contains("verify_with")) << "a tool result must not prescribe the next call";
}

// --- evaluate(): run Luau, get numbers back, leave nothing behind ----------

TEST(ToolRegistry, EvaluateHappyPathCreatesReadsAndRemoves) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  // Bridges the ephemeral create's resolved output back into `store`, so the
  // read_series-shaped read that follows create() has something to read.
  dp.ephemeral_series_store = &store;
  dp.ephemeral_series_ts = {0, kSec, 2 * kSec, 3 * kSec, 4 * kSec};
  dp.ephemeral_series_vals = {0.0, 2.0, 4.0, 6.0, 8.0};
  ToolContext ctx = makeCtx(store, &dp);
  int notify_calls = 0;
  ctx.notify_data_changed = [&]() { ++notify_calls; };

  auto r =
      reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}, {"buckets", 3}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.create_calls, 1);
  EXPECT_NE(dp.last_flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL, 0u);
  EXPECT_EQ(dp.persistent_creates, 0) << "an ephemeral node never joins the live set";
  EXPECT_EQ(dp.last_removed, dp.last_id) << "removed by the same id it was created under";
  EXPECT_EQ(notify_calls, 0) << "evaluate must never notify_data_changed -- nothing changed that the user can see";
  const json j = json::parse(r.content);
  EXPECT_TRUE(j.contains("stats"));
  EXPECT_TRUE(j.contains("buckets"));
}

TEST(ToolRegistry, EvaluateSurfacesValidateErrorWithoutCreating) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_validate = true;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value +"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 0);
}

// No bridge to a series for the resolved output: create succeeds, the read
// that follows it does not. The ephemeral node must still come out.
TEST(ToolRegistry, EvaluateRemovesTheEphemeralNodeEvenWhenTheReadFails) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}}, ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_EQ(dp.create_calls, 1);
  EXPECT_EQ(dp.last_removed, dp.last_id) << "the node must be removed even when the read after it fails";
}

TEST(ToolRegistry, EvaluateCallsUseDistinctIds) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);
  auto first = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}}, ctx);
  const std::string first_id = dp.last_id;
  auto second = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value * 3"}}, ctx);
  const std::string second_id = dp.last_id;
  (void)first;
  (void)second;
  EXPECT_FALSE(first_id.empty());
  EXPECT_NE(first_id, second_id);
}

// --- PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT: set when the SDK has it, with a
// fallback for a host that rejects the unknown bit -----------------------

#ifdef PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT

TEST(ToolRegistry, CreatesCarryTheHistoryExemptFlagWhenTheSdkExposesIt) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  {
    RecordingDpHost dp;
    dp.config_history_exempt = true;
    ToolContext ctx = makeCtx(store, &dp);
    auto r = reg.execute(
        "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_NE(dp.last_flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT, 0u);
    EXPECT_EQ(dp.config_calls, 1);
    EXPECT_EQ(dp.last_config_id, "x2");
    EXPECT_FALSE(json::parse(r.content).contains("undo_protection"));
  }
  {
    RecordingDpHost dp;
    dp.config_history_exempt = true;
    ToolContext ctx = makeCtx(store, &dp);
    auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_NE(dp.last_flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT, 0u);
    EXPECT_EQ(dp.config_calls, 1);
    EXPECT_EQ(dp.last_config_id, "assistant_markers");
    EXPECT_FALSE(json::parse(r.content).contains("undo_protection"));
  }
  {
    RecordingDpHost dp;
    dp.config_history_exempt = true;
    ToolContext ctx = makeCtx(store, &dp);
    const std::string rule = "local s = series(\"/imu/x\")\nstartMarker(0)\ncloseMarker(100)\n";
    auto r = reg.execute("create_markers", {{"inputs", json::array({"/imu/x"})}, {"rule", rule}}, ctx);
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_NE(dp.last_flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT, 0u);
    EXPECT_EQ(dp.config_calls, 1);
    EXPECT_EQ(dp.last_config_id, "assistant_markers");
    EXPECT_FALSE(json::parse(r.content).contains("undo_protection"));
  }
}

TEST(ToolRegistry, DisclosesWhenHistoryExemptIsFalseInTheCreatedRecipe) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.config_history_exempt = false;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable on this host");

  r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable on this host");

  const std::string rule = "local s = series(\"/imu/x\")\nstartMarker(0)\ncloseMarker(100)\n";
  r = reg.execute("create_markers", {{"inputs", json::array({"/imu/x"})}, {"rule", rule}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable on this host");
  EXPECT_EQ(dp.config_calls, 3) << "each persistent create site must verify the stored flag";
}

TEST(ToolRegistry, DisclosesWhenHistoryExemptIsMissingFromTheCreatedRecipe) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.config_calls, 1);
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable on this host");
}

TEST(ToolRegistry, DisclosesWhenTheCreatedRecipeCannotBeRead) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_config = true;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.config_calls, 1);
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable on this host");
}

TEST(ToolRegistry, DegradesOnceWhenAnOlderHostRejectsTheReservedBit) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.reject_unknown_flags = true;  // simulates a host that only knows EPHEMERAL
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.create_calls, 2) << "first call refused, retried once with flags cleared";
  EXPECT_EQ(dp.last_flags, 0u) << "the call that finally succeeded carried no flags";
  EXPECT_EQ(dp.config_calls, 0) << "the reserved-bit retry already established that protection is unavailable";
  const json j = json::parse(r.content);
  EXPECT_EQ(j["undo_protection"], "unavailable on this host");
}

#else

TEST(ToolRegistry, CreatesCarryNoHistoryExemptFlagWithoutSdkSupport) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_flags, 0u) << "no HISTORY_EXEMPT bit exists in this SDK build to set";
  EXPECT_EQ(dp.config_calls, 0) << "an SDK without the flag must not probe the recipe";

  r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_flags, 0u);
  EXPECT_EQ(dp.config_calls, 0);

  const std::string rule = "local s = series(\"/imu/x\")\nstartMarker(0)\ncloseMarker(100)\n";
  r = reg.execute("create_markers", {{"inputs", json::array({"/imu/x"})}, {"rule", rule}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_flags, 0u);
  EXPECT_EQ(dp.config_calls, 0);
}

#endif

// A rejection that is not the reserved-bit wording must not be retried --
// only a specific, matched refusal is worth a second attempt.
TEST(ToolRegistry, ANonReservedBitFailureIsNotRetried) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_create = true;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 1) << "a generic rejection must not trigger the flags=0 retry";
}

// --- several datasets loaded at once ---------------------------------------

// With two runs loaded, a flat topic list leaves the model unable to tell them
// apart — so it can neither offer a comparison nor avoid mixing them.
TEST(CatalogDigest, GroupsTopicsByDatasetWhenSeveralAreLoaded) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()));

  EXPECT_NE(digest.find("run_monday.mcap"), std::string::npos) << digest;
  EXPECT_NE(digest.find("run_friday.mcap"), std::string::npos) << digest;
  // The same topic name appears under both, which is the whole point: identical
  // names across runs are the norm, not a collision.
  EXPECT_LT(digest.find("run_monday.mcap"), digest.find("run_friday.mcap"));
  // The qualifier syntax is taught HERE, by the listing, not by the tool
  // schema: this line is paid for only in sessions that actually hold several
  // datasets, instead of in the prefix of every session.
  EXPECT_NE(digest.find("<dataset>:<topic>/<field>"), std::string::npos) << digest;
}

// --- addressing a series when several datasets are loaded -------------------

// The host's own display convention, "dataset:topic/field", addresses one
// run's series when both runs share every topic name.
TEST(ResolveSeriesPath, QualifiedPathPicksItsDataset) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto monday = assistant_agent::resolveSeriesPath(*catalog, "run_monday.mcap:/speed/value");
  const auto friday = assistant_agent::resolveSeriesPath(*catalog, "run_friday.mcap:/speed/value");
  ASSERT_TRUE(monday.resolved.has_value());
  ASSERT_TRUE(friday.resolved.has_value());
  EXPECT_NE(monday.resolved->handle.topic.id, friday.resolved->handle.topic.id)
      << "both qualifiers resolved to the same underlying series";
  EXPECT_EQ(monday.resolved->path, "run_monday.mcap:/speed/value");
}

// The failure the correctness sweep caught on screen: the bare path used to
// resolve silently to whichever file loaded first, for reads AND installs.
TEST(ResolveSeriesPath, RefusesABarePathThatExistsInSeveralDatasets) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = assistant_agent::resolveSeriesPath(*catalog, "/speed/value");
  EXPECT_FALSE(lookup.resolved.has_value()) << "resolved to '" << lookup.resolved->path << "' instead of refusing";
  EXPECT_TRUE(lookup.ambiguous);
  ASSERT_EQ(lookup.candidates.size(), 2u);
  // Candidates arrive in qualified form, copy-pasteable back as-is — this is
  // what turns the refusal into a one-round-trip correction.
  EXPECT_EQ(lookup.candidates[0], "run_monday.mcap:/speed/value");
  EXPECT_EQ(lookup.candidates[1], "run_friday.mcap:/speed/value");
}

// A topic that lives in only one of the datasets keeps resolving bare, and the
// resolved path discloses which dataset it came from.
TEST(ResolveSeriesPath, BarePathStillResolvesWhenUniqueAcrossDatasets) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu"}).addDataset("run_friday.mcap", {"/speed"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = assistant_agent::resolveSeriesPath(*catalog, "/speed/value");
  ASSERT_TRUE(lookup.resolved.has_value());
  EXPECT_EQ(lookup.resolved->path, "run_friday.mcap:/speed/value");
}

// Stream datasets carry names like "[stream] UDP Server" — spaces, brackets.
// Matching the qualifier against the known names (instead of parsing at ':')
// is what makes those work with no escaping rules.
TEST(ResolveSeriesPath, StreamStyleDatasetNamesNeedNoEscaping) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/udp/data"}).addDataset("[stream] UDP Server", {"/udp/data"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = assistant_agent::resolveSeriesPath(*catalog, "[stream] UDP Server:/udp/data/value");
  ASSERT_TRUE(lookup.resolved.has_value());
  EXPECT_EQ(lookup.resolved->path, "[stream] UDP Server:/udp/data/value");
}

// The host's create interface addresses inputs by BARE name, so a qualified
// create on a path that several datasets share cannot be delivered — the
// honest outcome is a loud refusal here, not an artifact on whichever dataset
// the host resolves first. (Reads are unaffected: they go by handle.)
TEST(ToolRegistry, CreateOnADuplicatedPathIsRefusedInsteadOfMistargeted) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  RecordingDpHost dp;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();

  auto r = reg.execute(
      "create_markers", {{"series", "run_friday.mcap:/speed/value"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  ASSERT_FALSE(r.ok) << r.content;
  EXPECT_NE(r.content.find("cannot target that specific dataset"), std::string::npos) << r.content;
  EXPECT_EQ(dp.create_calls, 0) << "nothing may reach the host after the refusal";
}

// When the bare name IS unique across datasets, a qualified create goes
// through — and what reaches the host is the bare form it understands, while
// the result reports the qualified form the model used.
TEST(ToolRegistry, QualifiedCreateOnAUniqueNameHandsTheHostItsBareForm) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu"}).addDataset("run_friday.mcap", {"/speed"});
  RecordingDpHost dp;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();

  auto r = reg.execute(
      "create_markers", {{"series", "run_friday.mcap:/speed/value"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_NE(dp.last_script.find("series(\"/speed/value\")"), std::string::npos) << dp.last_script;
  EXPECT_EQ(json::parse(r.content)["created_markers_on"], "run_friday.mcap:/speed/value");
}

// One dataset loaded: paths stay bare in both directions — no qualifier to
// learn, no extra characters in any result. The overwhelmingly common case
// pays nothing.
TEST(ResolveSeriesPath, SingleDatasetKeepsBarePaths) {
  FakeMultiDatasetHost host;
  host.addDataset("only.mcap", {"/imu", "/speed"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = assistant_agent::resolveSeriesPath(*catalog, "/speed/value");
  ASSERT_TRUE(lookup.resolved.has_value());
  EXPECT_EQ(lookup.resolved->path, "/speed/value");
}

// One dataset is the overwhelmingly common case and naming it every time buys
// nothing but tokens — on every API call, several times a turn.
TEST(CatalogDigest, SaysNothingAboutDatasetsWhenThereIsOnlyOne) {
  FakeMultiDatasetHost host;
  host.addDataset("only.mcap", {"/imu", "/speed"});

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()));

  EXPECT_EQ(digest.find("dataset"), std::string::npos) << digest;
  EXPECT_NE(digest.find("/imu"), std::string::npos) << digest;
}

TEST(ToolRegistry, DescribeTopicNamesItsDataset) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu"}).addDataset("run_friday.mcap", {"/speed"});
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());

  auto r = reg.execute("describe_topic", {{"topic", "/speed"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["dataset"], "run_friday.mcap");
}

TEST(ToolRegistry, ReportStatusNamesTheDatasets) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu"}).addDataset("run_friday.mcap", {"/speed"});
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());

  auto r = reg.execute("report_status", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["data_sources"], 2);
  EXPECT_EQ(j["dataset_names"][0], "run_monday.mcap");
  EXPECT_EQ(j["dataset_names"][1], "run_friday.mcap");
}

// --- one node, several outputs ---------------------------------------------

TEST(ToolRegistry, CreatesSeveralOutputsFromOneTransform) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series",
      {{"name", "split"},
       {"inputs", json::array({"/imu/x"})},
       {"outputs", json::array({"roll", "pitch", "yaw"})},
       {"body", "    return value, value * 2, value * 3"}},
      ctx);

  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_outputs, (std::vector<std::string>{"roll", "pitch", "yaw"}));
  const json j = json::parse(r.content);
  EXPECT_EQ(j["series"].size(), 3u);
  EXPECT_EQ(j["series"][0], "roll/value");
}

// Omitting `outputs` must behave exactly as before — this is the path every
// existing conversation takes.
TEST(ToolRegistry, DefaultsToASingleOutputNamedAfterTheSeries) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series", {{"name", "doubled"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}},
      ctx);

  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_outputs, (std::vector<std::string>{"doubled"}));
  EXPECT_EQ(json::parse(r.content)["series"], "doubled/value");
}

// --- what the tool surface costs, on every call ----------------------------

// The whole schema is re-sent on every API call, and one turn with tool use is
// several calls — so a description is not paid once per message, it is paid per
// round trip. This is a budget, not a style rule.
//
// Raised from 7500 to 8000 when batched reads landed, and the number is not a
// convenience: it was moved against a measurement. In the reference turn
// (41 round trips, 1.67 M tokens) a round trip costs ~40 700 tokens on average,
// and consecutive read runs account for 45% of the turn. The batch description
// adds ~95 tokens per round trip and removes roughly twenty of them — a trade of
// about 400 to 1.
//
// Move it again only with that kind of arithmetic behind it. Prose that cannot
// point at a saving is what the ceiling exists to stop, and when it binds the
// answer is to cut prose, not to drop a capability.
//
// Lowered back from 8000 to 7500 when the create_markers cautionary prose was
// compressed: -504 chars, measured as -39 prefix tokens in a same-day A/B (two
// app launches differing only in the plugin .so, identical prompt — fluent
// prose tokenizes far denser than chars/4 suggests). The line-wall warning kept
// its measured defense — the result reports marker count and kind, the
// loop-closing mechanism — and the A/B of 2026-08-11 showed the long prose
// bought nothing measurable. The threshold caution stays as one dense sentence:
// it is the only guidance with no corrective feedback behind it, and a GUI pass
// on the Nissan log confirmed the sentence still induces min-duration +
// slow-signal cross-check behavior (7 regions, no wall).
//
// Measured, not guessed: the chars/4 rule of thumb overestimated this surface by
// about 50% when it was checked against the real token counters.
//
// Lowered from 10500 to 9900 when zoom_to_time_range/zoom_reset were folded
// into plot_tab's 'zoom' action: two standalone tools (each paying the fixed
// JSON envelope below) collapsed into one action of an existing tool. Measured
// 9593 chars across 11 tools after the merge — down from two more tools' worth
// of envelope despite plot_tab's description covering six actions.
TEST(ToolRegistry, ToolSchemaStaysWithinItsBudget) {
  ToolRegistry reg;
  const std::size_t chars = reg.toFunctionSpecs().dump().size();
  std::cerr << "tool schema: " << chars << " chars across " << reg.tools().size() << " tools\n";
  for (const auto& t : reg.tools()) {
    std::cerr << "  " << t.name << ": " << t.description.size() << "\n";
  }
  // The whole tool surface is a fixed cost: it is sent ahead of every message
  // of every conversation, so a tool earns its characters or it does not
  // belong. That is what this ceiling is for — not a limit any model imposes,
  // but the one place where adding capability has to be a decision. Note the
  // fixed 73 chars of JSON envelope each tool costs before a word of prose,
  // which is why related verbs share one tool with an `action` argument
  // instead of standing alone. Raised for `evaluate` (run a Luau computation
  // without creating anything) — a new verb, not a variant of an existing
  // one, so it could not be folded into another tool's `action`.
  EXPECT_LT(chars, 10500u) << "the tool surface outgrew its budget — trim descriptions before adding capability";
}

// --- playback / viewport tools ----------------------------------------------

TEST(ToolRegistry, PlaybackToolsEchoFullState) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  for (const char* action : {"play", "pause", "state"}) {
    auto r = reg.execute("playback", {{"action", action}}, ctx);
    ASSERT_TRUE(r.ok) << action << ": " << r.content;
    auto j = json::parse(r.content);
    EXPECT_EQ(j["playing"], false) << action;
    EXPECT_DOUBLE_EQ(j["current_time_s"].get<double>(), 3.0) << action;
    EXPECT_DOUBLE_EQ(j["range"]["min_s"].get<double>(), 0.0) << action;
    EXPECT_DOUBLE_EQ(j["range"]["max_s"].get<double>(), 10.0) << action;
    EXPECT_DOUBLE_EQ(j["rate"].get<double>(), 1.0) << action;
  }
  EXPECT_TRUE(pb.play_called);
  EXPECT_TRUE(pb.pause_called);
}

TEST(ToolRegistry, SeekForwardsTimeAndValidatesArgs) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute("playback", {{"action", "seek"}, {"time_s", 7.25}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_DOUBLE_EQ(pb.last_seek_s, 7.25);

  // Missing / wrong-typed time_s -> clean failure, host untouched.
  pb.last_seek_s = -1.0;
  EXPECT_FALSE(reg.execute("playback", {{"action", "seek"}}, ctx).ok);
  EXPECT_FALSE(reg.execute("playback", {{"action", "seek"}, {"time_s", "later"}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_seek_s, -1.0);

  // Out-of-range seek: the echoed current_time_s is where the cursor LANDED
  // (the host clamps) — the contract the tool description promises.
  r = reg.execute("playback", {{"action", "seek"}, {"time_s", 999.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_DOUBLE_EQ(json::parse(r.content)["current_time_s"].get<double>(), 10.0);
}

TEST(ToolRegistry, SetPlaybackRateValidatesArgs) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  EXPECT_FALSE(reg.execute("playback", {{"action", "rate"}}, ctx).ok);
  EXPECT_FALSE(reg.execute("playback", {{"action", "rate"}, {"rate", "fast"}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, -1.0);  // host untouched
}

TEST(ToolRegistry, StateReadFailureDegradesCleanly) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  pb.fail_state = true;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  // action "state" converts the failed read into a tool failure.
  auto r = reg.execute("playback", {{"action", "state"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("state boom"), std::string::npos);

  // A mutation still succeeds; the echo self-describes the missing state.
  r = reg.execute("playback", {{"action", "play"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_TRUE(pb.play_called);
  EXPECT_NE(r.content.find("state_unavailable"), std::string::npos);
}

TEST(ToolRegistry, SetPlaybackRateClampsPluginSide) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  ASSERT_TRUE(reg.execute("playback", {{"action", "rate"}, {"rate", 0.25}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, 0.25);
  ASSERT_TRUE(reg.execute("playback", {{"action", "rate"}, {"rate", 10000.0}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, 20.0);  // clamped high
  ASSERT_TRUE(reg.execute("playback", {{"action", "rate"}, {"rate", 0.0}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, 0.05);  // clamped low (never 0 -> host reject loop)
}

TEST(ToolRegistry, PlaybackToolsDegradeWithoutHost) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);  // no playback host bound

  for (const char* action : {"play", "pause", "state"}) {
    auto r = reg.execute("playback", {{"action", action}}, ctx);
    EXPECT_FALSE(r.ok) << action;
    EXPECT_NE(r.content.find("pj.playback.v1"), std::string::npos) << action;
  }
  EXPECT_FALSE(reg.execute("playback", {{"action", "seek"}, {"time_s", 1.0}}, ctx).ok);
}

TEST(ToolRegistry, PlaybackRejectsAnUnknownAction) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute("playback", {{"action", "fly"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(pb.play_called);
  EXPECT_FALSE(pb.pause_called);
}

TEST(ToolRegistry, PlaybackRequiresAnAction) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  EXPECT_FALSE(reg.execute("playback", json::object(), ctx).ok);
}

// --- the assistant's own plot tabs ------------------------------------------
//
// plot_tab replaced zoom_to_time_range/zoom_reset as one tool with an 'action'
// argument. The product rule it exists to enforce: the model composes only in
// tabs it created, and the user's tabs are unreachable — a rule the HOST
// enforces (FakePlotTabsHost models that), not the plugin, so every test here
// drives the real ownership check rather than trusting the plugin's own
// bookkeeping.

TEST(ToolRegistry, PlotTabCreateReportsTheTabTheHostHolds) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();

  auto r = reg.execute("plot_tab", {{"action", "create"}, {"tab", "analysis"}, {"title", "Analysis"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["tab"], "analysis");
  EXPECT_EQ(j["title"], "Analysis");
  EXPECT_TRUE(j["curves"].empty());
}

TEST(ToolRegistry, PlotTabAddReportsWhatActuallyLanded) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  store.addTopic("/left");
  store.addField("/left", "speed", {0, kSec}, {1.0, 2.0});
  FakePlotTabsHost tabs;
  tabs.unresolvable.insert("/left");  // the host cannot place this one
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  auto r = reg.execute(
      "plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", json::array({"/imu/x", "/left/speed"})}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  // The host silently dropped /left/speed: the read-back shows only what
  // actually landed, so the dropped curve is absent, not silently claimed.
  ASSERT_EQ(j["curves"].size(), 1u);
  EXPECT_EQ(j["curves"][0]["topic"], "/imu");
  EXPECT_EQ(j["curves"][0]["field"], "x");

  // A path the catalog cannot resolve at all is a request error; when every
  // requested curve is like that, the call fails outright.
  auto none = reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "/does/not/exist"}}, ctx);
  EXPECT_FALSE(none.ok) << none.content;

  // The harder failure, and the one the read-back exists for: a path the
  // CATALOG resolves, which the host then accepts and silently places nowhere.
  // Every call returned success, so only the tab's own contents can tell the
  // truth — reporting this as a drawing would be the lie.
  auto dropped = reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "/left/speed"}}, ctx);
  EXPECT_FALSE(dropped.ok) << dropped.content;
  EXPECT_NE(dropped.content.find("did not land"), std::string::npos) << dropped.content;
}

// The product rule stated as a test: the host, not the plugin, is what keeps
// this assistant out of the user's own tabs.
TEST(ToolRegistry, PlotTabRefusesATabItDoesNotOwn) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  tabs.addForeignTab("user-1");
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();

  EXPECT_FALSE(
      reg.execute("plot_tab", {{"action", "add"}, {"tab", "user-1"}, {"curves", json::array({"/imu/x"})}}, ctx).ok);
  EXPECT_FALSE(
      reg.execute("plot_tab", {{"action", "remove"}, {"tab", "user-1"}, {"curves", json::array({"/imu/x"})}}, ctx).ok);
  // zoom never touches pj.plot_tabs.v1 at all (it goes through ctx.viewport,
  // left unbound here), so it fails on the missing service rather than ever
  // reaching the fake -- still "every one fails" as required.
  EXPECT_FALSE(reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "user-1"}}, ctx).ok);
  EXPECT_FALSE(reg.execute("plot_tab", {{"action", "close"}, {"tab", "user-1"}}, ctx).ok);

  EXPECT_EQ(tabs.foreign_mutations, 0);
}

// The structural version of the property above: loop over every registered
// tool (not just plot_tab) with a foreign tab named in its args, so this
// keeps holding automatically as tools are added later.
TEST(ToolRegistry, NoToolReachesAForeignTab) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  tabs.addForeignTab("user-1");
  RecordingDpHost dp;
  FakePlaybackHost pb;
  FakeViewportHost vp;
  auto ctx = makeCtx(store, &dp);
  ctx.plot_tabs = tabs.view();
  ctx.playback = pb.view();
  ctx.viewport = vp.view();

  const json plot_tab_args = {{"action", "add"}, {"tab", "user-1"}, {"curves", json::array({"/imu/x"})}};
  const json generic_args = {{"tab", "user-1"}};
  for (const auto& tool : reg.tools()) {
    std::ignore = reg.execute(tool.name, plot_tab_args, ctx);
    std::ignore = reg.execute(tool.name, generic_args, ctx);
  }

  EXPECT_EQ(tabs.foreign_mutations, 0) << "some tool call reached a tab this plugin does not own";
}

TEST(ToolRegistry, PlotTabListShowsOnlyItsOwn) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  tabs.addForeignTab("user-1");
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "a"}}, ctx).ok);
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "b"}}, ctx).ok);

  auto r = reg.execute("plot_tab", {{"action", "list"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["count"], 2);
  EXPECT_EQ(r.content.find("user-1"), std::string::npos) << r.content;
}

// Owning nothing is an answer, not an error.
TEST(ToolRegistry, PlotTabListWithNoTabsSucceeds) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();

  auto r = reg.execute("plot_tab", {{"action", "list"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["count"], 0);
}

TEST(ToolRegistry, PlotTabResolvesAbbreviatedCurves) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);  // /imu with fields x and y -- "x" alone is unambiguous
  FakePlotTabsHost tabs;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  auto r = reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "x"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  ASSERT_EQ(tabs.find("view")->curves.size(), 1u);
  EXPECT_EQ(tabs.find("view")->curves[0].topic, "/imu");
  EXPECT_EQ(tabs.find("view")->curves[0].field, "x");
}

TEST(ToolRegistry, PlotTabRefusesAnAmbiguousCurve) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  FakePlotTabsHost tabs;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  auto r = reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "/speed/value"}}, ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_NE(r.content.find("run_monday.mcap:/speed/value"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("run_friday.mcap:/speed/value"), std::string::npos) << r.content;
  EXPECT_TRUE(tabs.find("view")->curves.empty());
}

TEST(ToolRegistry, PlotTabQualifiedCurveCarriesItsDataset) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  FakePlotTabsHost tabs;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  auto r =
      reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "run_friday.mcap:/speed/value"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  ASSERT_EQ(tabs.find("view")->curves.size(), 1u);
  EXPECT_EQ(tabs.find("view")->curves[0].dataset, "run_friday.mcap");
}

TEST(ToolRegistry, PlotTabZoomValidatesItsRange) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  FakeViewportHost vp;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();
  ctx.viewport = vp.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  // start >= end is rejected BEFORE the host sees it.
  EXPECT_FALSE(
      reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "view"}, {"start_s", 9.0}, {"end_s", 1.0}}, ctx).ok);
  EXPECT_FALSE(
      reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "view"}, {"start_s", 5.0}, {"end_s", 5.0}}, ctx).ok);
  // Only one of start_s/end_s given is refused too.
  EXPECT_FALSE(reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "view"}, {"start_s", 1.0}}, ctx).ok);
  EXPECT_EQ(vp.zoom_calls, 0);

  // Neither given -> fit the view.
  auto r = reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "view"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_TRUE(vp.reset_called);
}

TEST(ToolRegistry, PlotTabDegradesWithoutTheService) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);  // ctx.plot_tabs left unbound

  auto r = reg.execute("plot_tab", {{"action", "list"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("pj.plot_tabs.v1"), std::string::npos) << r.content;
}

TEST(ToolRegistry, PlotTabUnknownActionIsACleanFailure) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();

  EXPECT_FALSE(reg.execute("plot_tab", {{"action", "levitate"}}, ctx).ok);
  EXPECT_FALSE(reg.execute("plot_tab", json::object(), ctx).ok);
}

TEST(ToolRegistry, ReadSeriesStatsGainDisplayStartWhenPlaybackBound) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  pb.display_offset_ns = -2'000'000'000;  // display = absolute + 2 s
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  ASSERT_TRUE(j["stats"].contains("t_start_display_s")) << r.content;
  // /imu/x starts at absolute 0 ns -> display 2.0 s under the fake offset.
  EXPECT_DOUBLE_EQ(j["stats"]["t_start_display_s"].get<double>(), 2.0);
  EXPECT_EQ(pb.last_topic, "/imu");  // converted against the OWNING topic

  // Without a playback host the field is absent (not an error).
  auto plain = makeCtx(store, nullptr);
  r = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, plain);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_FALSE(json::parse(r.content)["stats"].contains("t_start_display_s"));
}

// --- seeing and withdrawing its own work -----------------------------------

TEST(ToolRegistry, ListsWhatItHasCreated) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.live_ids = {"doubled", "assistant_markers"};
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute("list_created", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 2);
  EXPECT_EQ(j["created"][0], "doubled");
}

TEST(ToolRegistry, RemovesADerivedSeriesItCreated) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.live_ids = {"doubled"};
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute("remove_derived_series", {{"name", "doubled"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_removed, "doubled");
}

// The safety property, stated as a test rather than as a promise in a doc: a
// name this assistant never created is refused before the host is asked, and
// the refusal says what it DID create so the model can correct itself.
TEST(ToolRegistry, RefusesToRemoveSomethingItDidNotCreate) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.live_ids = {"doubled"};
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute("remove_derived_series", {{"name", "/imu/x"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(dp.last_removed.empty()) << "the host must not even be asked";
  EXPECT_NE(r.content.find("doubled"), std::string::npos) << r.content;
}

// --- reading several series in one call ------------------------------------

// The measured problem: in a real analysis the model issued 25 read_series, in
// runs of up to 15 back to back, and the CLI never batches two tools into one
// response — so each read cost a full round trip that re-sent the whole
// conversation. Those runs were 45% of the turn's tokens.
TEST(ToolRegistry, ReadsSeveralSeriesInOneCall) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  store.addField("/imu", "z", {0, kSec, 2 * kSec}, {7.0, 8.0, 9.0});
  ToolContext ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"paths", json::array({"/imu/x", "/imu/y", "/imu/z"})}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 3);
  EXPECT_EQ(j["read"].size(), 3u);
  EXPECT_EQ(j["read"][0]["series"], "/imu/x");
  EXPECT_EQ(j["read"][2]["series"], "/imu/z");
  // Each entry carries its own statistics, not a merged blob.
  EXPECT_EQ(j["read"][0]["stats"]["count"], 5);
  EXPECT_EQ(j["read"][1]["stats"]["count"], 3);
  EXPECT_FALSE(j.contains("failed"));
}

// One typo must not cost the round trip the batch exists to save.
TEST(ToolRegistry, ABadPathDoesNotSpoilTheBatch) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"paths", json::array({"/imu/x", "/imu/nope", "/imu/y"})}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 3);
  EXPECT_EQ(j["failed"], 1);
  EXPECT_TRUE(j["read"][0].contains("stats"));
  EXPECT_TRUE(j["read"][1].contains("error")) << j["read"][1].dump();
  EXPECT_TRUE(j["read"][2].contains("stats"));
}

// A single path keeps exactly the shape it had before batching existed, so
// nothing that already worked starts reading differently to the model.
TEST(ToolRegistry, SinglePathKeepsTheOldShape) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  auto batch = json::parse(reg.execute("read_series", {{"paths", json::array({"/imu/x"})}}, ctx).content);
  auto bare = json::parse(reg.execute("read_series", {{"paths", "/imu/x"}}, ctx).content);
  auto legacy = json::parse(reg.execute("read_series", {{"series", "/imu/x"}}, ctx).content);

  EXPECT_EQ(batch["series"], "/imu/x");
  EXPECT_FALSE(batch.contains("read")) << "a lone path must not be wrapped in the batch envelope";
  EXPECT_EQ(bare, batch);
  EXPECT_EQ(legacy, batch) << "'series' still works so a conversation in flight does not break";
}

// Buckets stay single-series on purpose: coarsening several shapes to fit one
// response destroys the only thing buckets are for. The refusal has to say so
// and point at what does work, or the model just retries the same thing.
TEST(ToolRegistry, BucketsRefuseABatchAndSayWhy) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"paths", json::array({"/imu/x", "/imu/y"})}, {"mode", "buckets"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("one series at a time"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("stats"), std::string::npos) << r.content;
}

TEST(ToolRegistry, RejectsAnOversizedBatch) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  json many = json::array();
  for (int i = 0; i < 40; ++i) {
    many.push_back("/imu/x");
  }
  auto r = reg.execute("read_series", {{"paths", many}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("at most"), std::string::npos) << r.content;
}

}  // namespace

// A name that is already taken is not a judgement call: installing over it
// shadows something the user already has, and "create" was not asked to replace
// anything. The tool description used to send the model to list_created "before
// creating something that may already exist" — an instruction it had to
// remember, in place of a check that costs nothing.
TEST(ToolRegistry, RefusesToCreateOverAnExistingName) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  const json args = {{"name", "doubled"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}};
  ASSERT_TRUE(reg.execute("create_derived_series", args, ctx).ok);
  ASSERT_EQ(dp.liveCount(), 1);

  const auto again = reg.execute("create_derived_series", args, ctx);
  EXPECT_FALSE(again.ok) << "creating the same name twice must not silently install a second one";
  EXPECT_NE(again.content.find("already exists"), std::string::npos) << again.content;
  EXPECT_EQ(dp.liveCount(), 1) << "the refused create must leave the first one untouched";
  EXPECT_EQ(dp.create_calls, 1) << "and must not reach the host at all";
}
