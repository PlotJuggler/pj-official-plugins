#define _USE_MATH_DEFINES

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/host/toolbox_library.hpp"
#include "pj_plugins/testing/toolbox_test_store.hpp"
#include "quaternion_to_rpy.hpp"

#ifndef PJ_QUATERNION_PLUGIN_PATH
#error "PJ_QUATERNION_PLUGIN_PATH must be defined"
#endif

namespace {

// ---------------------------------------------------------------------------
// Fake pj.data_processors.v1 host
//
// Captures the single create_data_processor call the plugin makes, so the test
// can assert the data-only contract (inputs/outputs/script) without a real
// DerivedEngine. The actual Luau execution is validated end-to-end inside PJ4.
// ---------------------------------------------------------------------------
struct FakeDataProcessorsHost {
  int create_calls = 0;
  int remove_calls = 0;
  bool fail = false;  // when set, create_data_processor reports failure (e.g. inputs unresolved)
  std::string id;
  std::string script;
  std::string params;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::vector<std::string> live_ids;  // ids the host currently considers installed

  // Simulate the host tearing every node down (session clear / dataset reload).
  void clearNodes() {
    live_ids.clear();
  }

  static bool createImpl(
      void* ctx, PJ_string_view_t id, PJ_string_view_t /*kind*/, PJ_string_view_t /*language*/,
      const PJ_string_view_t* inputs, uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count,
      PJ_string_view_t script, PJ_string_view_t params, uint32_t /*flags*/, PJ_string_view_t* /*out_topics*/,
      uint64_t /*out_topics_capacity*/, uint64_t* out_topics_count, PJ_error_t* /*out_error*/) noexcept {
    auto* self = static_cast<FakeDataProcessorsHost*>(ctx);
    self->create_calls++;
    if (self->fail) {
      return false;
    }
    self->id.assign(id.data, id.size);
    self->script.assign(script.data, script.size);
    self->params.assign(params.data, params.size);
    self->inputs.clear();
    for (uint64_t i = 0; i < input_count; ++i) {
      self->inputs.emplace_back(inputs[i].data, inputs[i].size);
    }
    self->outputs.clear();
    for (uint64_t i = 0; i < output_count; ++i) {
      self->outputs.emplace_back(outputs[i].data, outputs[i].size);
    }
    if (std::find(self->live_ids.begin(), self->live_ids.end(), self->id) == self->live_ids.end()) {
      self->live_ids.push_back(self->id);  // upsert by id
    }
    if (out_topics_count != nullptr) {
      *out_topics_count = self->outputs.size();
    }
    return true;
  }

  static bool removeImpl(void* ctx, PJ_string_view_t id, PJ_error_t* /*out_error*/) noexcept {
    auto* self = static_cast<FakeDataProcessorsHost*>(ctx);
    self->remove_calls++;
    std::string id_str(id.data, id.size);
    self->live_ids.erase(std::remove(self->live_ids.begin(), self->live_ids.end(), id_str), self->live_ids.end());
    return true;
  }

  // Count-then-fill enumeration of live ids (see list_data_processor_ids ABI).
  static bool listImpl(
      void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count,
      PJ_error_t* /*out_error*/) noexcept {
    auto* self = static_cast<FakeDataProcessorsHost*>(ctx);
    *out_count = self->live_ids.size();
    for (uint64_t i = 0; i < self->live_ids.size() && i < capacity; ++i) {
      out_ids[i].data = self->live_ids[i].data();
      out_ids[i].size = self->live_ids[i].size();
    }
    return true;
  }

  PJ_data_processors_host_t makeHost() {
    vtable_.protocol_version = 1;
    vtable_.struct_size = sizeof(PJ_data_processors_host_vtable_t);
    vtable_.create_data_processor = &createImpl;
    vtable_.remove_data_processor = &removeImpl;
    vtable_.list_data_processor_ids = &listImpl;
    vtable_.data_processor_config = nullptr;
    return PJ_data_processors_host_t{this, &vtable_};
  }

 private:
  PJ_data_processors_host_vtable_t vtable_{};
};

// Bind toolbox + runtime + data-processors services to the handle.
void bindStore(
    PJ::ToolboxHandle& handle, PJ::testing::ToolboxTestStore& store, FakeDataProcessorsHost& dp,
    PJ::ServiceRegistryBuilder& registry) {
  registry.registerService<PJ::sdk::ToolboxHostService>(store.makeHost());
  registry.registerService<PJ::sdk::ToolboxRuntimeHostService>(store.makeRuntimeHost());
  registry.registerService<PJ::sdk::DataProcessorsHostService>(dp.makeHost());
  ASSERT_TRUE(handle.bind(registry.view()));
}

TEST(QuaternionPluginTest, LoadsAndHasCorrectManifest) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  EXPECT_TRUE(library->valid());

  auto handle = library->createHandle();
  EXPECT_TRUE(handle.valid());
  EXPECT_NE(handle.manifest().find("Quaternion to RPY"), std::string::npos);
}

// The plugin now hands the host a Luau class instead of computing RPY itself.
// Assert the data-only contract: field-level inputs in w,x,y,z order, ONE grouped
// output topic ("<prefix>:roll,pitch,yaw"), and a script carrying the conversion +
// the chosen options.
TEST(QuaternionPluginTest, InstallsDataProcessorWithFieldInputs) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;
  FakeDataProcessorsHost dp;
  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, dp, registry);

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "quat/x", "input_y": "quat/y",
    "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "rpy/", "unwrap": true, "degrees": true
  })"));

  EXPECT_EQ(dp.create_calls, 1);
  // Inputs are FIELD paths in w, x, y, z order (value=w, v1=x, v2=y, v3=z).
  ASSERT_EQ(dp.inputs.size(), 4u);
  EXPECT_EQ(dp.inputs[0], "quat/w");
  EXPECT_EQ(dp.inputs[1], "quat/x");
  EXPECT_EQ(dp.inputs[2], "quat/y");
  EXPECT_EQ(dp.inputs[3], "quat/z");
  // ONE grouped output: topic "rpy" with roll/pitch/yaw as named columns.
  ASSERT_EQ(dp.outputs.size(), 1u);
  EXPECT_EQ(dp.outputs[0], "rpy:roll,pitch,yaw");
  EXPECT_EQ(dp.id, "rpy");
  // The script ports the converter math and bakes degrees + unwrap.
  EXPECT_NE(dp.script.find("math.atan2"), std::string::npos);
  EXPECT_NE(dp.script.find("math.asin"), std::string::npos);
  EXPECT_NE(dp.script.find("SCALE = 180.0 / math.pi"), std::string::npos);
  EXPECT_NE(dp.script.find("UNWRAP = true"), std::string::npos);
  EXPECT_EQ(store.notifyDataChangedCalls(), 1);
}

TEST(QuaternionPluginTest, RadianAndUnwrapOffAreBakedIntoScript) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;
  FakeDataProcessorsHost dp;
  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, dp, registry);

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "q/x", "input_y": "q/y",
    "input_z": "q/z", "input_w": "q/w",
    "output_prefix": "out/", "unwrap": false, "degrees": false
  })"));

  EXPECT_EQ(dp.create_calls, 1);
  EXPECT_EQ(dp.outputs.front(), "out:roll,pitch,yaw");
  EXPECT_NE(dp.script.find("SCALE = 1.0"), std::string::npos);
  EXPECT_NE(dp.script.find("UNWRAP = false"), std::string::npos);
}

// A second config with a different output prefix retargets: the old node is
// removed before the new one is created, so no orphan lingers.
TEST(QuaternionPluginTest, RetargetsOnPrefixChange) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;
  FakeDataProcessorsHost dp;
  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, dp, registry);

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "quat/x", "input_y": "quat/y", "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "rpy/", "unwrap": true, "degrees": true
  })"));
  EXPECT_EQ(dp.create_calls, 1);
  EXPECT_EQ(dp.remove_calls, 0);

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "quat/x", "input_y": "quat/y", "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "euler/", "unwrap": true, "degrees": true
  })"));
  EXPECT_EQ(dp.create_calls, 2);
  EXPECT_EQ(dp.remove_calls, 1);  // dropped "rpy" before creating "euler"
  EXPECT_EQ(dp.id, "euler");
}

// A session clear / dataset reload tears the host node down while the plugin's
// created_ flag stays set. A data change while the node is alive must NOT churn,
// but once it is gone (no longer listed) the next data change must reinstall it.
TEST(QuaternionPluginTest, ReinstallsAfterHostNodeTornDown) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;
  FakeDataProcessorsHost dp;
  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, dp, registry);

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "quat/x", "input_y": "quat/y", "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "rpy/", "unwrap": true, "degrees": true
  })"));
  EXPECT_EQ(dp.create_calls, 1);
  ASSERT_EQ(dp.live_ids.size(), 1u);

  // Node still listed -> no reinstall.
  handle.onDataChanged();
  EXPECT_EQ(dp.create_calls, 1);

  // Host tears the node down -> next data change reinstalls it.
  dp.clearNodes();
  handle.onDataChanged();
  EXPECT_EQ(dp.create_calls, 2);
  ASSERT_EQ(dp.live_ids.size(), 1u);
}

// A ':' or ',' in the output prefix would corrupt the grouped-output spec
// ("topic:f1,f2,f3"); they are neutralized before the spec is built.
TEST(QuaternionPluginTest, SanitizesColonAndCommaInOutputPrefix) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;
  FakeDataProcessorsHost dp;
  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, dp, registry);

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "q/x", "input_y": "q/y", "input_z": "q/z", "input_w": "q/w",
    "output_prefix": "a:b,c/", "unwrap": true, "degrees": true
  })"));
  ASSERT_EQ(dp.outputs.size(), 1u);
  EXPECT_EQ(dp.outputs[0], "a_b_c:roll,pitch,yaw");
  EXPECT_EQ(dp.id, "a_b_c");
}

// A layout restored before its data loads: the config is valid but the host
// cannot create the node yet (inputs unresolvable). loadConfig must still
// succeed — the apply is best-effort and onDataChanged reinstalls once the
// data arrives — instead of surfacing a spurious config-load failure.
TEST(QuaternionPluginTest, LoadConfigDefersWhenCreateFails) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;
  FakeDataProcessorsHost dp;
  dp.fail = true;  // host rejects the create
  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, dp, registry);

  EXPECT_TRUE(handle.loadConfig(R"({
    "input_x": "q/x", "input_y": "q/y", "input_z": "q/z", "input_w": "q/w",
    "output_prefix": "rpy/", "unwrap": true, "degrees": true
  })"));
  EXPECT_EQ(dp.create_calls, 1);  // it tried, but the failure did not fail loadConfig
}

TEST(QuaternionPluginTest, ConfigRoundTrip) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  std::string config =
      R"({"degrees":true,"input_w":"w","input_x":"x","input_y":"y","input_z":"z","output_prefix":"out/","unwrap":true})";
  ASSERT_TRUE(handle.loadConfig(config));
  std::string out_config;
  ASSERT_TRUE(handle.saveConfig(out_config));
  EXPECT_EQ(out_config, config);
}

// ---------------------------------------------------------------------------
// Converter math (the source the generated Luau ports verbatim, and what the
// dialog preview runs). The Luau-on-host equivalence is covered end-to-end in
// PJ4; these guard the C++ converter the script mirrors.
// ---------------------------------------------------------------------------

TEST(QuaternionConverterTest, IdentityProducesZeroRPY) {
  QuaternionToRPYConverter conv;
  conv.setScale(QuaternionToRPYConverter::kDegPerRad);
  conv.setUnwrap(true);
  conv.reset();

  std::array<double, 3> rpy{};
  conv.convert(0, {0.0, 0.0, 0.0, 1.0}, rpy);
  EXPECT_NEAR(rpy[0], 0.0, 1e-9);
  EXPECT_NEAR(rpy[1], 0.0, 1e-9);
  EXPECT_NEAR(rpy[2], 0.0, 1e-9);
}

TEST(QuaternionConverterTest, NinetyDegreeRotations) {
  constexpr double s = 0.7071067811865476;  // sin(45) = cos(45)
  QuaternionToRPYConverter conv;
  conv.setScale(QuaternionToRPYConverter::kDegPerRad);
  conv.setUnwrap(false);
  conv.reset();

  std::array<double, 3> rpy{};
  // Sample 0: 90 roll.
  conv.convert(0, {s, 0.0, 0.0, s}, rpy);
  EXPECT_NEAR(rpy[0], 90.0, 0.01);
  EXPECT_NEAR(rpy[1], 0.0, 0.01);
  EXPECT_NEAR(rpy[2], 0.0, 0.01);

  // Sample 1: 90 pitch (gimbal lock — roll/yaw decompose to 180, equivalent).
  conv.convert(1, {0.0, s, 0.0, s}, rpy);
  EXPECT_NEAR(rpy[0], 180.0, 0.01);
  EXPECT_NEAR(rpy[1], 90.0, 0.01);
  EXPECT_NEAR(rpy[2], 180.0, 0.01);

  // Sample 2: 90 yaw.
  conv.convert(2, {0.0, 0.0, s, s}, rpy);
  EXPECT_NEAR(rpy[0], 0.0, 0.01);
  EXPECT_NEAR(rpy[1], 0.0, 0.01);
  EXPECT_NEAR(rpy[2], 90.0, 0.01);
}

TEST(QuaternionConverterTest, RadianOutput) {
  constexpr double s = 0.7071067811865476;
  QuaternionToRPYConverter conv;
  conv.setScale(1.0);
  conv.setUnwrap(false);
  conv.reset();

  std::array<double, 3> rpy{};
  conv.convert(0, {s, 0.0, 0.0, s}, rpy);
  EXPECT_NEAR(rpy[0], M_PI / 2.0, 1e-4);
  EXPECT_NEAR(rpy[1], 0.0, 1e-4);
  EXPECT_NEAR(rpy[2], 0.0, 1e-4);
}

}  // namespace
