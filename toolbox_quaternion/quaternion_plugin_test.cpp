#define _USE_MATH_DEFINES

#include <gtest/gtest.h>

#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/host/toolbox_library.hpp"

#ifndef PJ_QUATERNION_PLUGIN_PATH
#error "PJ_QUATERNION_PLUGIN_PATH must be defined"
#endif

namespace {

// ---------------------------------------------------------------------------
// Minimal Arrow C Data Interface builder — just enough to let the toolbox
// plugin's readSeries() helper walk the resulting struct. We emit two
// children: an int64 "timestamp" column and a float64 value column.
//
// Schema and array have DISJOINT ownership — each carries its own private_data
// heap block and its own release callback frees only that block. This keeps
// their lifetimes independent, which matters because MaterializedSeriesView's
// holders destroy the array before the schema.
// ---------------------------------------------------------------------------

struct ArrowSchemaPayload {
  ArrowSchema child_ts{};
  ArrowSchema child_val{};
  ArrowSchema* child_ptrs[2]{};
};

struct ArrowArrayPayload {
  std::vector<int64_t> timestamps;
  std::vector<double> values;
  ArrowArray child_ts{};
  ArrowArray child_val{};
  ArrowArray* child_ptrs[2]{};
  const void* ts_buffers[2]{};
  const void* val_buffers[2]{};
};

inline void releaseArrowSchema(ArrowSchema* schema) noexcept {
  auto* p = static_cast<ArrowSchemaPayload*>(schema->private_data);
  delete p;
  schema->release = nullptr;
  schema->private_data = nullptr;
  schema->children = nullptr;
  schema->n_children = 0;
}

inline void releaseArrowArray(ArrowArray* array) noexcept {
  auto* p = static_cast<ArrowArrayPayload*>(array->private_data);
  delete p;
  array->release = nullptr;
  array->private_data = nullptr;
  array->children = nullptr;
  array->n_children = 0;
}

inline void buildArrowSeries(
    const std::vector<int64_t>& ts, const std::vector<double>& vals, ArrowSchema* out_schema, ArrowArray* out_array) {
  auto* sp = new ArrowSchemaPayload{};
  sp->child_ts = ArrowSchema{
      .format = "l",
      .name = "timestamp",
      .metadata = nullptr,
      .flags = 0,
      .n_children = 0,
      .children = nullptr,
      .dictionary = nullptr,
      .release = [](ArrowSchema* s) noexcept { s->release = nullptr; },
      .private_data = nullptr};
  sp->child_val = ArrowSchema{
      .format = "g",
      .name = "value",
      .metadata = nullptr,
      .flags = 0,
      .n_children = 0,
      .children = nullptr,
      .dictionary = nullptr,
      .release = [](ArrowSchema* s) noexcept { s->release = nullptr; },
      .private_data = nullptr};
  sp->child_ptrs[0] = &sp->child_ts;
  sp->child_ptrs[1] = &sp->child_val;

  *out_schema = ArrowSchema{
      .format = "+s",
      .name = "",
      .metadata = nullptr,
      .flags = 0,
      .n_children = 2,
      .children = sp->child_ptrs,
      .dictionary = nullptr,
      .release = releaseArrowSchema,
      .private_data = sp};

  auto* ap = new ArrowArrayPayload{};
  ap->timestamps = ts;
  ap->values = vals;
  ap->ts_buffers[0] = nullptr;
  ap->ts_buffers[1] = ap->timestamps.data();
  ap->val_buffers[0] = nullptr;
  ap->val_buffers[1] = ap->values.data();

  const int64_t length = static_cast<int64_t>(ap->values.size());
  ap->child_ts = ArrowArray{
      .length = length,
      .null_count = 0,
      .offset = 0,
      .n_buffers = 2,
      .n_children = 0,
      .buffers = ap->ts_buffers,
      .children = nullptr,
      .dictionary = nullptr,
      .release = [](ArrowArray* a) noexcept { a->release = nullptr; },
      .private_data = nullptr};
  ap->child_val = ArrowArray{
      .length = length,
      .null_count = 0,
      .offset = 0,
      .n_buffers = 2,
      .n_children = 0,
      .buffers = ap->val_buffers,
      .children = nullptr,
      .dictionary = nullptr,
      .release = [](ArrowArray* a) noexcept { a->release = nullptr; },
      .private_data = nullptr};
  ap->child_ptrs[0] = &ap->child_ts;
  ap->child_ptrs[1] = &ap->child_val;

  *out_array = ArrowArray{
      .length = length,
      .null_count = 0,
      .offset = 0,
      .n_buffers = 0,
      .n_children = 2,
      .buffers = nullptr,
      .children = ap->child_ptrs,
      .dictionary = nullptr,
      .release = releaseArrowArray,
      .private_data = ap};
}

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
// v4 toolbox host vtable — all trampolines noexcept, PJ_error_t out-param.
// ---------------------------------------------------------------------------

static bool hostCreateDataSource(
    void* ctx, PJ_string_view_t, PJ_data_source_handle_t* out, PJ_error_t*) noexcept {
  auto* store = static_cast<MockDataStore*>(ctx);
  ++store->create_data_source_calls;
  *out = PJ_data_source_handle_t{1};
  return true;
}

static bool hostEnsureTopic(
    void*, PJ_data_source_handle_t, PJ_string_view_t, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
  *out = PJ_topic_handle_t{100};
  return true;
}

static bool hostEnsureField(
    void*, PJ_topic_handle_t, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* out, PJ_error_t*) noexcept {
  *out = PJ_field_handle_t{PJ_topic_handle_t{100}, 1};
  return true;
}

static bool hostAppendRecord(
    void* ctx, PJ_topic_handle_t, int64_t timestamp, const PJ_named_field_value_t* fields, size_t field_count,
    PJ_error_t*) noexcept {
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

static bool hostAppendBoundRecord(
    void*, PJ_topic_handle_t, int64_t, const PJ_bound_field_value_t*, size_t, PJ_error_t*) noexcept {
  return true;
}

static bool hostAppendArrowStream(
    void*, PJ_topic_handle_t, struct ArrowArrayStream* stream, PJ_string_view_t, PJ_error_t*) noexcept {
  if (stream != nullptr && stream->release != nullptr) {
    stream->release(stream);
  }
  return true;
}

struct CatalogRelease {
  PJ_topic_info_t* topics;
  PJ_field_info_t* fields;
};

static bool hostAcquireCatalogSnapshot(
    void* ctx, PJ_catalog_snapshot_t* out, PJ_error_t*) noexcept {
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

static bool hostReadSeriesArrow(
    void* ctx, PJ_field_handle_t field, struct ArrowSchema* out_schema, struct ArrowArray* out_array,
    PJ_error_t*) noexcept {
  auto* store = static_cast<MockDataStore*>(ctx);
  const FieldEntry* entry = store->findField(field);
  if (!entry || entry->values.empty()) return false;
  buildArrowSeries(entry->timestamps, entry->values, out_schema, out_array);
  return true;
}

PJ_toolbox_host_t makeToolboxHost(MockDataStore* store) {
  static const PJ_toolbox_host_vtable_t vtable = {
      .abi_version = PJ_PLUGIN_DATA_API_VERSION,
      .struct_size = sizeof(PJ_toolbox_host_vtable_t),
      .create_data_source = hostCreateDataSource,
      .ensure_topic = hostEnsureTopic,
      .ensure_field = hostEnsureField,
      .append_record = hostAppendRecord,
      .append_bound_record = hostAppendBoundRecord,
      .append_arrow_stream = hostAppendArrowStream,
      .acquire_catalog_snapshot = hostAcquireCatalogSnapshot,
      .read_series_arrow = hostReadSeriesArrow,
  };
  return PJ_toolbox_host_t{.ctx = store, .vtable = &vtable};
}

// ---------------------------------------------------------------------------
// Runtime host vtable — v4 signatures.
// ---------------------------------------------------------------------------

static void runtimeReportMessage(void*, PJ_toolbox_message_level_t, PJ_string_view_t) noexcept {}

static void runtimeNotifyDataChanged(void* ctx) noexcept {
  auto* store = static_cast<MockDataStore*>(ctx);
  ++store->notify_data_changed_calls;
}

PJ_toolbox_runtime_host_t makeRuntimeHost(MockDataStore* store) {
  static const PJ_toolbox_runtime_host_vtable_t vtable = {
      .protocol_version = PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,
      .struct_size = sizeof(PJ_toolbox_runtime_host_vtable_t),
      .report_message = runtimeReportMessage,
      .notify_data_changed = runtimeNotifyDataChanged,
  };
  return PJ_toolbox_runtime_host_t{.ctx = store, .vtable = &vtable};
}

// Convenience: bind both services to a registry and bind() the handle.
void bindStoreServices(PJ::ToolboxHandle& handle, MockDataStore& store, PJ::ServiceRegistryBuilder& registry) {
  registry.registerService<PJ::sdk::ToolboxHostService>(makeToolboxHost(&store));
  registry.registerService<PJ::sdk::ToolboxRuntimeHostService>(makeRuntimeHost(&store));
  ASSERT_TRUE(handle.bind(registry.view()));
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

  PJ::ServiceRegistryBuilder registry;
  bindStoreServices(handle, store, registry);

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

  PJ::ServiceRegistryBuilder registry;
  bindStoreServices(handle, store, registry);

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

  PJ::ServiceRegistryBuilder registry;
  bindStoreServices(handle, store, registry);

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

  PJ::ServiceRegistryBuilder registry;
  bindStoreServices(handle, store, registry);

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
  std::string out_config;
  ASSERT_TRUE(handle.saveConfig(out_config));
  EXPECT_EQ(out_config, config);
}

}  // namespace
