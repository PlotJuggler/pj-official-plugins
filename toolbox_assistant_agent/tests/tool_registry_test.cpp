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
#include <cstdint>
#include <nlohmann/json.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>
#include <vector>

#include "support/recording_dp_host.hpp"

namespace {

using assistant_agent::catalogDigest;
using assistant_agent::ToolContext;
using assistant_agent::ToolRegistry;
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

ToolContext makeCtx(PJ::testing::ToolboxTestStore& store, RecordingDpHost* dp) {
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(store.makeHost());
  if (dp != nullptr) {
    ctx.dp = dp->view();
  }
  return ctx;
}

TEST(ToolRegistry, ListsAllToolsAndSchemas) {
  ToolRegistry reg;
  EXPECT_EQ(reg.tools().size(), 7u);
  // Both serializations expose every tool by name.
  EXPECT_EQ(reg.toOllamaTools().size(), 7u);
  EXPECT_EQ(reg.toMcpToolsList().size(), 7u);
  EXPECT_NE(reg.find("create_derived_series"), nullptr);
  // remove_markers exists but is scoped to the assistant's own marker set;
  // no tool can touch user data destructively.
  EXPECT_NE(reg.find("remove_markers"), nullptr);
  EXPECT_EQ(reg.find("delete_everything"), nullptr);
  // The assistant reads and creates; it never drives the app.
  EXPECT_EQ(reg.find("seek"), nullptr);
  EXPECT_EQ(reg.find("zoom_to_time_range"), nullptr);
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

}  // namespace
