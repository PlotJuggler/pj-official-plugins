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

namespace {

using assistant_agent::ToolContext;
using assistant_agent::ToolRegistry;
using nlohmann::json;

constexpr std::int64_t kSec = 1'000'000'000;

// Records the last create/validate call so tests can assert the synthesized
// Luau script + routed inputs/outputs. Returns success unless a fail flag is set.
struct RecordingDpHost {
  int create_calls = 0;
  int validate_calls = 0;
  bool fail_validate = false;
  bool fail_create = false;
  std::string last_kind;
  std::string last_id;
  std::string last_removed;
  std::string last_script;
  std::string last_validate_script;
  std::vector<std::string> last_inputs;
  std::vector<std::string> last_outputs;
  std::vector<std::string> resolved;  // storage the returned borrowed views point into

  static std::string toStr(PJ_string_view_t v) {
    return std::string(v.data == nullptr ? "" : v.data, v.size);
  }

  static bool tCreate(
      void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t language, const PJ_string_view_t* inputs,
      uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count, PJ_string_view_t script,
      PJ_string_view_t /*params*/, uint32_t /*flags*/, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
      uint64_t* out_topics_count, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    (void)language;
    ++self->create_calls;
    self->last_id = toStr(id);
    self->last_kind = toStr(kind);
    self->last_script = toStr(script);
    self->last_inputs.clear();
    for (uint64_t i = 0; i < input_count; ++i) {
      self->last_inputs.push_back(toStr(inputs[i]));
    }
    self->last_outputs.clear();
    for (uint64_t i = 0; i < output_count; ++i) {
      self->last_outputs.push_back(toStr(outputs[i]));
    }
    if (self->fail_create) {
      PJ::sdk::fillError(err, 1, "test", "create rejected");
      return false;
    }
    self->resolved = self->last_outputs.empty() ? std::vector<std::string>{"auto_topic"} : self->last_outputs;
    if (out_topics_count != nullptr) {
      *out_topics_count = self->resolved.size();
    }
    for (uint64_t i = 0; i < self->resolved.size() && i < out_topics_capacity; ++i) {
      out_topics[i] = PJ_string_view_t{self->resolved[i].data(), self->resolved[i].size()};
    }
    return true;
  }

  static bool tValidate(
      void* ctx, PJ_string_view_t /*kind*/, PJ_string_view_t /*language*/, PJ_string_view_t script,
      PJ_string_view_t /*params*/, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    ++self->validate_calls;
    self->last_validate_script = toStr(script);
    if (self->fail_validate) {
      PJ::sdk::fillError(err, 1, "test", "compile error");
      return false;
    }
    return true;
  }

  static bool tRemove(void* ctx, PJ_string_view_t id, PJ_error_t* /*err*/) noexcept {
    static_cast<RecordingDpHost*>(ctx)->last_removed = toStr(id);
    return true;
  }

  PJ::sdk::DataProcessorsHostView view() {
    static const PJ_data_processors_host_vtable_t vtable = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_data_processors_host_vtable_t),
        .create_data_processor = &RecordingDpHost::tCreate,
        .remove_data_processor = &RecordingDpHost::tRemove,
        .list_data_processor_ids = nullptr,
        .data_processor_config = nullptr,
        .validate_data_processor_script = &RecordingDpHost::tValidate,
    };
    return PJ::sdk::DataProcessorsHostView(PJ_data_processors_host_t{this, &vtable});
  }
};

// Fake pj.playback.v1 host: records the last call, serves a settable state,
// and converts absolute ns -> display seconds with a fixed offset so the
// read_series enrichment is observable.
struct FakePlaybackHost {
  bool play_called = false;
  bool pause_called = false;
  double last_seek_s = -1.0;
  double last_rate = -1.0;
  PJ_playback_state_t state{
      .is_playing = false, .current_time_s = 3.0, .range_min_s = 0.0, .range_max_s = 10.0, .playback_rate = 1.0};
  bool fail_state = false;
  std::string last_topic;
  std::int64_t display_offset_ns = 0;

  static bool tPlay(void* ctx, PJ_error_t*) noexcept {
    static_cast<FakePlaybackHost*>(ctx)->play_called = true;
    return true;
  }
  static bool tPause(void* ctx, PJ_error_t*) noexcept {
    static_cast<FakePlaybackHost*>(ctx)->pause_called = true;
    return true;
  }
  static bool tSeek(void* ctx, double time_s, PJ_error_t*) noexcept {
    auto* self = static_cast<FakePlaybackHost*>(ctx);
    self->last_seek_s = time_s;
    // Mirror the real host: clamp into range and reflect it in the state the
    // subsequent get_state echo reads.
    self->state.current_time_s = std::clamp(time_s, self->state.range_min_s, self->state.range_max_s);
    return true;
  }
  static bool tSetRate(void* ctx, double rate, PJ_error_t*) noexcept {
    static_cast<FakePlaybackHost*>(ctx)->last_rate = rate;
    return true;
  }
  static bool tGetState(void* ctx, PJ_playback_state_t* out_state, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlaybackHost*>(ctx);
    if (self->fail_state) {
      PJ::sdk::fillError(err, 2, "fake", "state boom");
      return false;
    }
    *out_state = self->state;
    return true;
  }
  static bool tToDisplayTime(
      void* ctx, PJ_string_view_t topic, std::int64_t absolute_ns, double* out_display_s, PJ_error_t*) noexcept {
    auto* self = static_cast<FakePlaybackHost*>(ctx);
    self->last_topic = std::string(PJ::sdk::toStringView(topic));
    *out_display_s = static_cast<double>(absolute_ns - self->display_offset_ns) * 1e-9;
    return true;
  }

  PJ::sdk::PlaybackHostView view() {
    static const PJ_playback_host_vtable_t vtable = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_playback_host_vtable_t),
        .play = &FakePlaybackHost::tPlay,
        .pause = &FakePlaybackHost::tPause,
        .seek = &FakePlaybackHost::tSeek,
        .set_playback_rate = &FakePlaybackHost::tSetRate,
        .get_state = &FakePlaybackHost::tGetState,
        .to_display_time = &FakePlaybackHost::tToDisplayTime,
    };
    return PJ::sdk::PlaybackHostView(PJ_playback_host_t{this, &vtable});
  }
};

// Fake pj.viewport.v1 host: records the last zoom call.
struct FakeViewportHost {
  int zoom_calls = 0;
  double last_t0_s = 0.0;
  double last_t1_s = 0.0;
  bool reset_called = false;

  static bool tZoom(void* ctx, double t0_s, double t1_s, PJ_error_t*) noexcept {
    auto* self = static_cast<FakeViewportHost*>(ctx);
    ++self->zoom_calls;
    self->last_t0_s = t0_s;
    self->last_t1_s = t1_s;
    return true;
  }
  static bool tReset(void* ctx, PJ_error_t*) noexcept {
    static_cast<FakeViewportHost*>(ctx)->reset_called = true;
    return true;
  }

  PJ::sdk::ViewportHostView view() {
    static const PJ_viewport_host_vtable_t vtable = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_viewport_host_vtable_t),
        .zoom_to_time_range = &FakeViewportHost::tZoom,
        .zoom_reset = &FakeViewportHost::tReset,
    };
    return PJ::sdk::ViewportHostView(PJ_viewport_host_t{this, &vtable});
  }
};

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
  EXPECT_EQ(reg.tools().size(), 14u);
  // Both serializations expose every tool by name.
  EXPECT_EQ(reg.toOllamaTools().size(), 14u);
  EXPECT_EQ(reg.toMcpToolsList().size(), 14u);
  EXPECT_NE(reg.find("create_derived_series"), nullptr);
  EXPECT_NE(reg.find("seek"), nullptr);
  EXPECT_NE(reg.find("zoom_to_time_range"), nullptr);
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

// --- playback / viewport tools ----------------------------------------------

TEST(ToolRegistry, PlaybackToolsEchoFullState) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  for (const char* tool : {"play", "pause", "get_playback_state"}) {
    auto r = reg.execute(tool, json::object(), ctx);
    ASSERT_TRUE(r.ok) << tool << ": " << r.content;
    auto j = json::parse(r.content);
    EXPECT_EQ(j["playing"], false) << tool;
    EXPECT_DOUBLE_EQ(j["current_time_s"].get<double>(), 3.0) << tool;
    EXPECT_DOUBLE_EQ(j["range"]["min_s"].get<double>(), 0.0) << tool;
    EXPECT_DOUBLE_EQ(j["range"]["max_s"].get<double>(), 10.0) << tool;
    EXPECT_DOUBLE_EQ(j["rate"].get<double>(), 1.0) << tool;
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

  auto r = reg.execute("seek", {{"time_s", 7.25}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_DOUBLE_EQ(pb.last_seek_s, 7.25);

  // Missing / wrong-typed time_s -> clean failure, host untouched.
  pb.last_seek_s = -1.0;
  EXPECT_FALSE(reg.execute("seek", json::object(), ctx).ok);
  EXPECT_FALSE(reg.execute("seek", {{"time_s", "later"}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_seek_s, -1.0);

  // Out-of-range seek: the echoed current_time_s is where the cursor LANDED
  // (the host clamps) — the contract the tool description promises.
  r = reg.execute("seek", {{"time_s", 999.0}}, ctx);
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

  EXPECT_FALSE(reg.execute("set_playback_rate", json::object(), ctx).ok);
  EXPECT_FALSE(reg.execute("set_playback_rate", {{"rate", "fast"}}, ctx).ok);
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

  // get_playback_state converts the failed read into a tool failure.
  auto r = reg.execute("get_playback_state", json::object(), ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("state boom"), std::string::npos);

  // A mutation still succeeds; the echo self-describes the missing state.
  r = reg.execute("play", json::object(), ctx);
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

  ASSERT_TRUE(reg.execute("set_playback_rate", {{"rate", 0.25}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, 0.25);
  ASSERT_TRUE(reg.execute("set_playback_rate", {{"rate", 10000.0}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, 20.0);  // clamped high
  ASSERT_TRUE(reg.execute("set_playback_rate", {{"rate", 0.0}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, 0.05);  // clamped low (never 0 -> host reject loop)
}

TEST(ToolRegistry, PlaybackToolsDegradeWithoutHost) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);  // no playback host bound

  for (const char* tool : {"play", "pause", "get_playback_state"}) {
    auto r = reg.execute(tool, json::object(), ctx);
    EXPECT_FALSE(r.ok) << tool;
    EXPECT_NE(r.content.find("pj.playback.v1"), std::string::npos) << tool;
  }
  EXPECT_FALSE(reg.execute("seek", {{"time_s", 1.0}}, ctx).ok);
}

TEST(ToolRegistry, ZoomForwardsRangeAndValidates) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakeViewportHost vp;
  auto ctx = makeCtx(store, nullptr);
  ctx.viewport = vp.view();

  auto r = reg.execute("zoom_to_time_range", {{"start_s", 2.0}, {"end_s", 8.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(vp.zoom_calls, 1);
  EXPECT_DOUBLE_EQ(vp.last_t0_s, 2.0);
  EXPECT_DOUBLE_EQ(vp.last_t1_s, 8.5);
  auto j = json::parse(r.content);
  EXPECT_DOUBLE_EQ(j["zoomed"]["start_s"].get<double>(), 2.0);

  // start >= end is rejected BEFORE the host sees it.
  EXPECT_FALSE(reg.execute("zoom_to_time_range", {{"start_s", 9.0}, {"end_s", 1.0}}, ctx).ok);
  EXPECT_FALSE(reg.execute("zoom_to_time_range", {{"start_s", 5.0}, {"end_s", 5.0}}, ctx).ok);
  EXPECT_FALSE(reg.execute("zoom_to_time_range", {{"start_s", 1.0}}, ctx).ok);  // missing end_s
  EXPECT_EQ(vp.zoom_calls, 1);
}

TEST(ToolRegistry, ZoomResetForwardsAndDegrades) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakeViewportHost vp;
  auto ctx = makeCtx(store, nullptr);
  ctx.viewport = vp.view();

  ASSERT_TRUE(reg.execute("zoom_reset", json::object(), ctx).ok);
  EXPECT_TRUE(vp.reset_called);

  auto unbound = makeCtx(store, nullptr);
  auto r = reg.execute("zoom_reset", json::object(), unbound);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("pj.viewport.v1"), std::string::npos);
}

TEST(ToolRegistry, ZoomEchoesPlaybackStateWhenBound) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  FakeViewportHost vp;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();
  ctx.viewport = vp.view();

  auto r = reg.execute("zoom_to_time_range", {{"start_s", 0.0}, {"end_s", 1.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  ASSERT_TRUE(j.contains("playback"));
  EXPECT_DOUBLE_EQ(j["playback"]["current_time_s"].get<double>(), 3.0);
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
