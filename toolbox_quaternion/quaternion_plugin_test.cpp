#define _USE_MATH_DEFINES

#include <gtest/gtest.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/host/toolbox_library.hpp"
#include "pj_plugins/testing/toolbox_test_store.hpp"

#ifndef PJ_QUATERNION_PLUGIN_PATH
#error "PJ_QUATERNION_PLUGIN_PATH must be defined"
#endif

namespace {

// Bind both toolbox + runtime services to the handle.
void bindStore(PJ::ToolboxHandle& handle, PJ::testing::ToolboxTestStore& store, PJ::ServiceRegistryBuilder& registry) {
  registry.registerService<PJ::sdk::ToolboxHostService>(store.makeHost());
  registry.registerService<PJ::sdk::ToolboxRuntimeHostService>(store.makeRuntimeHost());
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

TEST(QuaternionPluginTest, IdentityQuaternionProducesZeroRPY) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;
  std::vector<int64_t> ts = {0, 1000000000};  // 0s, 1s in nanoseconds
  store.addTopic("quat")
      .addField("quat", "x", ts, {0.0, 0.0})
      .addField("quat", "y", ts, {0.0, 0.0})
      .addField("quat", "z", ts, {0.0, 0.0})
      .addField("quat", "w", ts, {1.0, 1.0});

  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, registry);

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "quat/x", "input_y": "quat/y",
    "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "rpy/", "unwrap": true, "degrees": true
  })"));

  EXPECT_EQ(store.notifyDataChangedCalls(), 1);
  auto flat = store.flatRecords();
  ASSERT_EQ(flat.size(), 6u);  // 2 samples x 3 fields (roll, pitch, yaw)

  printf("\n=== Identity Quaternion (0,0,0,1) -> RPY (degrees) ===\n");
  for (const auto& r : flat) {
    printf("  ts=%" PRId64 "  %-6s = %8.3f\n", r.timestamp, r.field_name.c_str(), r.numeric);
  }

  // All values should be 0.
  for (const auto& r : flat) {
    EXPECT_NEAR(r.numeric, 0.0, 1e-9) << r.field_name;
  }
}

TEST(QuaternionPluginTest, NinetyDegreeRotations) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;
  constexpr double s = 0.7071067811865476;  // sin(45) = cos(45) = 1/sqrt(2)

  // Sample 0: 90 roll  (0.707, 0, 0, 0.707)
  // Sample 1: 90 pitch (0, 0.707, 0, 0.707)
  // Sample 2: 90 yaw   (0, 0, 0.707, 0.707)
  std::vector<int64_t> ts = {0, 1000000000, 2000000000};
  store.addTopic("quat")
      .addField("quat", "x", ts, {s, 0.0, 0.0})
      .addField("quat", "y", ts, {0.0, s, 0.0})
      .addField("quat", "z", ts, {0.0, 0.0, s})
      .addField("quat", "w", ts, {s, s, s});

  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, registry);

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "quat/x", "input_y": "quat/y",
    "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "rpy/", "unwrap": false, "degrees": true
  })"));

  auto flat = store.flatRecords();
  ASSERT_EQ(flat.size(), 9u);  // 3 samples x 3 fields

  printf("\n=== 90-degree Rotations -> RPY (degrees) ===\n");
  for (size_t i = 0; i < flat.size(); i += 3) {
    printf(
        "  Sample %zu: roll=%8.3f  pitch=%8.3f  yaw=%8.3f\n", i / 3, flat[i].numeric, flat[i + 1].numeric,
        flat[i + 2].numeric);
  }

  // Sample 0: 90 roll
  EXPECT_NEAR(flat[0].numeric, 90.0, 0.01);  // roll
  EXPECT_NEAR(flat[1].numeric, 0.0, 0.01);   // pitch
  EXPECT_NEAR(flat[2].numeric, 0.0, 0.01);   // yaw

  // Sample 1: 90 pitch (gimbal lock — roll and yaw are ambiguous,
  // atan2 decomposes to roll=180, pitch=90, yaw=180 which is equivalent)
  EXPECT_NEAR(flat[3].numeric, 180.0, 0.01);  // roll (gimbal lock artifact)
  EXPECT_NEAR(flat[4].numeric, 90.0, 0.01);   // pitch
  EXPECT_NEAR(flat[5].numeric, 180.0, 0.01);  // yaw (gimbal lock artifact)

  // Sample 2: 90 yaw
  EXPECT_NEAR(flat[6].numeric, 0.0, 0.01);   // roll
  EXPECT_NEAR(flat[7].numeric, 0.0, 0.01);   // pitch
  EXPECT_NEAR(flat[8].numeric, 90.0, 0.01);  // yaw
}

TEST(QuaternionPluginTest, RadianOutput) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;
  constexpr double s = 0.7071067811865476;
  std::vector<int64_t> ts = {0};

  store.addTopic("q")
      .addField("q", "x", ts, {s})
      .addField("q", "y", ts, {0.0})
      .addField("q", "z", ts, {0.0})
      .addField("q", "w", ts, {s});

  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, registry);

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "q/x", "input_y": "q/y",
    "input_z": "q/z", "input_w": "q/w",
    "output_prefix": "out/", "unwrap": false, "degrees": false
  })"));

  auto flat = store.flatRecords();
  ASSERT_EQ(flat.size(), 3u);

  printf("\n=== 90-degree Roll -> RPY (radians) ===\n");
  printf("  roll=%8.5f  pitch=%8.5f  yaw=%8.5f\n", flat[0].numeric, flat[1].numeric, flat[2].numeric);

  EXPECT_NEAR(flat[0].numeric, M_PI / 2.0, 0.0001);  // roll = pi/2
  EXPECT_NEAR(flat[1].numeric, 0.0, 0.0001);
  EXPECT_NEAR(flat[2].numeric, 0.0, 0.0001);
}

TEST(QuaternionPluginTest, IncrementalProcessing) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  PJ::testing::ToolboxTestStore store;

  // Start with 2 identity quaternion samples.
  std::vector<int64_t> ts = {0, 1000000000};
  store.addTopic("quat")
      .addField("quat", "x", ts, {0.0, 0.0})
      .addField("quat", "y", ts, {0.0, 0.0})
      .addField("quat", "z", ts, {0.0, 0.0})
      .addField("quat", "w", ts, {1.0, 1.0});

  PJ::ServiceRegistryBuilder registry;
  bindStore(handle, store, registry);

  std::string config = R"({
    "input_x": "quat/x", "input_y": "quat/y",
    "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "rpy/", "unwrap": true, "degrees": true
  })";

  // First call: processes 2 samples.
  ASSERT_TRUE(handle.loadConfig(config));
  EXPECT_EQ(store.createDataSourceCalls(), 1);
  ASSERT_EQ(store.flatRecords().size(), 6u);  // 2 samples x 3 fields
  EXPECT_EQ(store.notifyDataChangedCalls(), 1);

  // Simulate new data arriving: append a third sample (90-degree roll).
  constexpr double s = 0.7071067811865476;
  store.extendField("x", {2000000000}, {s})
      .extendField("y", {2000000000}, {0.0})
      .extendField("z", {2000000000}, {0.0})
      .extendField("w", {2000000000}, {s});

  // Second call with same config: should only process the new sample.
  ASSERT_TRUE(handle.loadConfig(config));
  EXPECT_EQ(store.createDataSourceCalls(), 1);  // NOT 2 — reuses the existing data source
  auto flat = store.flatRecords();
  ASSERT_EQ(flat.size(), 9u);  // 6 previous + 3 new (1 sample x 3 fields)
  EXPECT_EQ(store.notifyDataChangedCalls(), 2);

  // The new sample should be a 90-degree roll.
  EXPECT_NEAR(flat[6].numeric, 90.0, 0.01);  // roll
  EXPECT_NEAR(flat[7].numeric, 0.0, 0.01);   // pitch
  EXPECT_NEAR(flat[8].numeric, 0.0, 0.01);   // yaw
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

}  // namespace
