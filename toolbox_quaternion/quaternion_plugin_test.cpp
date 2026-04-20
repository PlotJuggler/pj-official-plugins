#define _USE_MATH_DEFINES

#include <gtest/gtest.h>

#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_plugins/host/toolbox_library.hpp"

#ifndef PJ_QUATERNION_PLUGIN_PATH
#error "PJ_QUATERNION_PLUGIN_PATH must be defined"
#endif

namespace {

// ---------------------------------------------------------------------------
// Mock data store — holds input quaternion series and captures output records.
// ---------------------------------------------------------------------------

struct FieldEntry {
  std::string name;
  PJ_field_handle_t handle;
  std::vector<double> values;
  std::vector<int64_t> timestamps;
};

struct OutputRecord {
  int64_t timestamp;
  std::string field_name;
  double value;
};

struct TopicEntry {
  std::string name;
  uint32_t topic_id;
  uint32_t first_field;
  uint32_t field_count;
};

struct MockDataStore {
  std::vector<FieldEntry> fields;
  std::vector<TopicEntry> topics;
  std::vector<OutputRecord> output;
  int create_data_source_calls = 0;
  int notify_data_changed_calls = 0;

  void addTopic(const std::string& name, uint32_t topic_id, uint32_t first_field, uint32_t field_count) {
    topics.push_back({name, topic_id, first_field, field_count});
  }

  void addField(const std::string& name, uint32_t topic_id, uint32_t field_id,
                const std::vector<double>& values, const std::vector<int64_t>& timestamps) {
    fields.push_back({name, PJ_field_handle_t{PJ_topic_handle_t{topic_id}, field_id},
                      values, timestamps});
  }

  const FieldEntry* findField(PJ_field_handle_t handle) const {
    for (const auto& f : fields) {
      if (f.handle.topic.id == handle.topic.id && f.handle.id == handle.id) {
        return &f;
      }
    }
    return nullptr;
  }
};

// ---------------------------------------------------------------------------
// Toolbox host vtable callbacks
// ---------------------------------------------------------------------------

static const char* hostGetLastError(void*) { return nullptr; }

static bool hostCreateDataSource(void* ctx, PJ_string_view_t, PJ_data_source_handle_t* out) {
  auto* store = static_cast<MockDataStore*>(ctx);
  ++store->create_data_source_calls;
  *out = PJ_data_source_handle_t{1};
  return true;
}

static bool hostEnsureTopic(void*, PJ_data_source_handle_t, PJ_string_view_t, PJ_topic_handle_t* out) {
  *out = PJ_topic_handle_t{100};
  return true;
}

static bool hostEnsureField(void*, PJ_topic_handle_t, PJ_string_view_t, PJ_primitive_type_t,
                            PJ_field_handle_t* out) {
  *out = PJ_field_handle_t{PJ_topic_handle_t{100}, 1};
  return true;
}

static bool hostAppendRecord(void* ctx, PJ_topic_handle_t, int64_t timestamp,
                             const PJ_named_field_value_t* fields, size_t field_count) {
  auto* store = static_cast<MockDataStore*>(ctx);
  for (size_t i = 0; i < field_count; ++i) {
    store->output.push_back({
        timestamp,
        std::string(fields[i].name.data, fields[i].name.size),
        fields[i].value.data.as_float64,
    });
  }
  return true;
}

static bool hostAppendBoundRecord(void*, PJ_topic_handle_t, int64_t, const PJ_bound_field_value_t*, size_t) {
  return true;
}

static bool hostAppendArrowIpc(void*, PJ_topic_handle_t, PJ_bytes_view_t, PJ_string_view_t) {
  return true;
}

struct CatalogRelease {
  PJ_topic_info_t* topics;
  PJ_field_info_t* fields;
};

static bool hostAcquireCatalogSnapshot(void* ctx, PJ_catalog_snapshot_t* out) {
  auto* store = static_cast<MockDataStore*>(ctx);

  auto* field_infos = new PJ_field_info_t[store->fields.size()];
  for (size_t i = 0; i < store->fields.size(); ++i) {
    field_infos[i].handle = store->fields[i].handle;
    field_infos[i].name = PJ_string_view_t{store->fields[i].name.data(), store->fields[i].name.size()};
    field_infos[i].type = PJ_PRIMITIVE_TYPE_FLOAT64;
  }

  auto* topic_infos = new PJ_topic_info_t[store->topics.size()];
  for (size_t i = 0; i < store->topics.size(); ++i) {
    topic_infos[i].handle = PJ_topic_handle_t{store->topics[i].topic_id};
    topic_infos[i].source = PJ_data_source_handle_t{1};
    topic_infos[i].name = PJ_string_view_t{store->topics[i].name.data(), store->topics[i].name.size()};
    topic_infos[i].first_field = store->topics[i].first_field;
    topic_infos[i].field_count = store->topics[i].field_count;
  }

  out->data_sources = nullptr;
  out->data_source_count = 0;
  out->topics = topic_infos;
  out->topic_count = store->topics.size();
  out->fields = field_infos;
  out->field_count = store->fields.size();
  auto* rel = new CatalogRelease{topic_infos, field_infos};
  out->release_ctx = rel;
  out->release = [](void* p) {
    auto* r = static_cast<CatalogRelease*>(p);
    delete[] r->topics;
    delete[] r->fields;
    delete r;
  };
  return true;
}

static bool hostReadSeries(void* ctx, PJ_field_handle_t field, PJ_materialized_series_t* out) {
  auto* store = static_cast<MockDataStore*>(ctx);
  const FieldEntry* entry = store->findField(field);
  if (!entry || entry->values.empty()) return false;

  out->source = PJ_data_source_handle_t{1};
  out->topic = field.topic;
  out->field = field;
  out->type = PJ_PRIMITIVE_TYPE_FLOAT64;
  out->timestamps = entry->timestamps.data();
  out->row_count = entry->values.size();
  out->validity_bits = nullptr;
  out->validity_size = 0;
  out->values.as_float64 = entry->values.data();
  out->release_ctx = nullptr;
  out->release = nullptr;
  return true;
}

PJ_toolbox_host_t makeToolboxHost(MockDataStore* store) {
  static const PJ_toolbox_host_vtable_t vtable = {
      .abi_version = PJ_PLUGIN_DATA_API_VERSION,
      .struct_size = sizeof(PJ_toolbox_host_vtable_t),
      .get_last_error = hostGetLastError,
      .create_data_source = hostCreateDataSource,
      .ensure_topic = hostEnsureTopic,
      .ensure_field = hostEnsureField,
      .append_record = hostAppendRecord,
      .append_bound_record = hostAppendBoundRecord,
      .append_arrow_ipc = hostAppendArrowIpc,
      .acquire_catalog_snapshot = hostAcquireCatalogSnapshot,
      .read_series = hostReadSeries,
  };
  return PJ_toolbox_host_t{.ctx = store, .vtable = &vtable};
}

// ---------------------------------------------------------------------------
// Runtime host vtable callbacks
// ---------------------------------------------------------------------------

static const char* runtimeGetLastError(void*) { return nullptr; }
static void runtimeReportMessage(void*, PJ_toolbox_message_level_t, PJ_string_view_t) {}

static void runtimeNotifyDataChanged(void* ctx) {
  auto* store = static_cast<MockDataStore*>(ctx);
  ++store->notify_data_changed_calls;
}

PJ_toolbox_runtime_host_t makeRuntimeHost(MockDataStore* store) {
  static const PJ_toolbox_runtime_host_vtable_t vtable = {
      .protocol_version = PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,
      .struct_size = sizeof(PJ_toolbox_runtime_host_vtable_t),
      .get_last_error = runtimeGetLastError,
      .report_message = runtimeReportMessage,
      .notify_data_changed = runtimeNotifyDataChanged,
  };
  return PJ_toolbox_runtime_host_t{.ctx = store, .vtable = &vtable};
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

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

  MockDataStore store;
  std::vector<int64_t> ts = {0, 1000000000};  // 0s, 1s in nanoseconds

  store.addTopic("quat", 1, 0, 4);
  store.addField("x", 1, 1, {0.0, 0.0}, ts);
  store.addField("y", 1, 2, {0.0, 0.0}, ts);
  store.addField("z", 1, 3, {0.0, 0.0}, ts);
  store.addField("w", 1, 4, {1.0, 1.0}, ts);

  ASSERT_TRUE(handle.bindToolboxHost(makeToolboxHost(&store)));
  ASSERT_TRUE(handle.bindRuntimeHost(makeRuntimeHost(&store)));

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "quat/x", "input_y": "quat/y",
    "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "rpy/", "unwrap": true, "degrees": true
  })"));

  EXPECT_EQ(store.notify_data_changed_calls, 1);
  ASSERT_EQ(store.output.size(), 6u);  // 2 samples x 3 fields (roll, pitch, yaw)

  printf("\n=== Identity Quaternion (0,0,0,1) -> RPY (degrees) ===\n");
  for (const auto& r : store.output) {
    printf("  ts=%" PRId64 "  %-6s = %8.3f\n", r.timestamp, r.field_name.c_str(), r.value);
  }

  // All values should be 0.
  for (const auto& r : store.output) {
    EXPECT_NEAR(r.value, 0.0, 1e-9) << r.field_name;
  }
}

TEST(QuaternionPluginTest, NinetyDegreeRotations) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  MockDataStore store;
  constexpr double s = 0.7071067811865476;  // sin(45) = cos(45) = 1/sqrt(2)

  // Sample 0: 90 roll  (0.707, 0, 0, 0.707)
  // Sample 1: 90 pitch (0, 0.707, 0, 0.707)
  // Sample 2: 90 yaw   (0, 0, 0.707, 0.707)
  std::vector<int64_t> ts = {0, 1000000000, 2000000000};

  store.addTopic("quat", 1, 0, 4);
  store.addField("x", 1, 1, {s, 0.0, 0.0}, ts);
  store.addField("y", 1, 2, {0.0, s, 0.0}, ts);
  store.addField("z", 1, 3, {0.0, 0.0, s}, ts);
  store.addField("w", 1, 4, {s, s, s}, ts);

  ASSERT_TRUE(handle.bindToolboxHost(makeToolboxHost(&store)));
  ASSERT_TRUE(handle.bindRuntimeHost(makeRuntimeHost(&store)));

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "quat/x", "input_y": "quat/y",
    "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "rpy/", "unwrap": false, "degrees": true
  })"));

  ASSERT_EQ(store.output.size(), 9u);  // 3 samples x 3 fields

  printf("\n=== 90-degree Rotations -> RPY (degrees) ===\n");
  for (size_t i = 0; i < store.output.size(); i += 3) {
    printf("  Sample %zu: roll=%8.3f  pitch=%8.3f  yaw=%8.3f\n",
           i / 3, store.output[i].value, store.output[i + 1].value, store.output[i + 2].value);
  }

  // Sample 0: 90 roll
  EXPECT_NEAR(store.output[0].value, 90.0, 0.01);   // roll
  EXPECT_NEAR(store.output[1].value, 0.0, 0.01);    // pitch
  EXPECT_NEAR(store.output[2].value, 0.0, 0.01);    // yaw

  // Sample 1: 90 pitch (gimbal lock — roll and yaw are ambiguous,
  // atan2 decomposes to roll=180, pitch=90, yaw=180 which is equivalent)
  EXPECT_NEAR(store.output[3].value, 180.0, 0.01);  // roll (gimbal lock artifact)
  EXPECT_NEAR(store.output[4].value, 90.0, 0.01);   // pitch
  EXPECT_NEAR(store.output[5].value, 180.0, 0.01);  // yaw (gimbal lock artifact)

  // Sample 2: 90 yaw
  EXPECT_NEAR(store.output[6].value, 0.0, 0.01);    // roll
  EXPECT_NEAR(store.output[7].value, 0.0, 0.01);    // pitch
  EXPECT_NEAR(store.output[8].value, 90.0, 0.01);   // yaw
}

TEST(QuaternionPluginTest, RadianOutput) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  MockDataStore store;
  constexpr double s = 0.7071067811865476;
  std::vector<int64_t> ts = {0};

  store.addTopic("q", 1, 0, 4);
  store.addField("x", 1, 1, {s}, ts);
  store.addField("y", 1, 2, {0.0}, ts);
  store.addField("z", 1, 3, {0.0}, ts);
  store.addField("w", 1, 4, {s}, ts);

  ASSERT_TRUE(handle.bindToolboxHost(makeToolboxHost(&store)));
  ASSERT_TRUE(handle.bindRuntimeHost(makeRuntimeHost(&store)));

  ASSERT_TRUE(handle.loadConfig(R"({
    "input_x": "q/x", "input_y": "q/y",
    "input_z": "q/z", "input_w": "q/w",
    "output_prefix": "out/", "unwrap": false, "degrees": false
  })"));

  ASSERT_EQ(store.output.size(), 3u);

  printf("\n=== 90-degree Roll -> RPY (radians) ===\n");
  printf("  roll=%8.5f  pitch=%8.5f  yaw=%8.5f\n",
         store.output[0].value, store.output[1].value, store.output[2].value);

  EXPECT_NEAR(store.output[0].value, M_PI / 2.0, 0.0001);  // roll = pi/2
  EXPECT_NEAR(store.output[1].value, 0.0, 0.0001);
  EXPECT_NEAR(store.output[2].value, 0.0, 0.0001);
}

TEST(QuaternionPluginTest, IncrementalProcessing) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  MockDataStore store;

  // Start with 2 identity quaternion samples.
  store.addTopic("quat", 1, 0, 4);
  store.addField("x", 1, 1, {0.0, 0.0}, {0, 1000000000});
  store.addField("y", 1, 2, {0.0, 0.0}, {0, 1000000000});
  store.addField("z", 1, 3, {0.0, 0.0}, {0, 1000000000});
  store.addField("w", 1, 4, {1.0, 1.0}, {0, 1000000000});

  ASSERT_TRUE(handle.bindToolboxHost(makeToolboxHost(&store)));
  ASSERT_TRUE(handle.bindRuntimeHost(makeRuntimeHost(&store)));

  std::string config = R"({
    "input_x": "quat/x", "input_y": "quat/y",
    "input_z": "quat/z", "input_w": "quat/w",
    "output_prefix": "rpy/", "unwrap": true, "degrees": true
  })";

  // First call: processes 2 samples.
  ASSERT_TRUE(handle.loadConfig(config));
  EXPECT_EQ(store.create_data_source_calls, 1);
  ASSERT_EQ(store.output.size(), 6u);  // 2 samples x 3 fields
  EXPECT_EQ(store.notify_data_changed_calls, 1);

  // Simulate new data arriving: append a third sample (90-degree roll).
  constexpr double s = 0.7071067811865476;
  for (auto& f : store.fields) {
    f.timestamps.push_back(2000000000);
    if (f.name == "x") {
      f.values.push_back(s);
    } else if (f.name == "w") {
      f.values.push_back(s);
    } else {
      f.values.push_back(0.0);
    }
  }

  // Second call with same config: should only process the new sample.
  ASSERT_TRUE(handle.loadConfig(config));
  EXPECT_EQ(store.create_data_source_calls, 1);  // NOT 2 — reuses the existing data source
  ASSERT_EQ(store.output.size(), 9u);  // 6 previous + 3 new (1 sample x 3 fields)
  EXPECT_EQ(store.notify_data_changed_calls, 2);

  // The new sample should be a 90-degree roll.
  EXPECT_NEAR(store.output[6].value, 90.0, 0.01);   // roll
  EXPECT_NEAR(store.output[7].value, 0.0, 0.01);    // pitch
  EXPECT_NEAR(store.output[8].value, 0.0, 0.01);    // yaw
}

TEST(QuaternionPluginTest, ConfigRoundTrip) {
  auto library = PJ::ToolboxLibrary::load(PJ_QUATERNION_PLUGIN_PATH);
  ASSERT_TRUE(library) << library.error();
  auto handle = library->createHandle();

  std::string config = R"({"degrees":true,"input_w":"w","input_x":"x","input_y":"y","input_z":"z","output_prefix":"out/","unwrap":true})";
  ASSERT_TRUE(handle.loadConfig(config));
  EXPECT_EQ(handle.saveConfig(), config);
}

}  // namespace
