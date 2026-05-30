// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// End-to-end regression on REAL server data. Loads the Arrow IPC batch
// captured from demo.mosaico.dev (`/camera/jai/rgb/image`, ontology `image`)
// and runs it through the real producer (`pushImageRowsToHost`), then
// deserializes every pushed blob with the pj_base canonical codec and asserts
// it reconstructs a valid per-frame PJ::Image.
//
// This guards the seam that unit tests approximate with synthetic tables: the
// actual on-wire layout uses Arrow `*_view` columns (BINARY_VIEW / STRING_VIEW)
// and ships a PNG-wrapped Bayer mosaic (format=png, encoding=bayer_rggb8,
// 1296x966, stride=1296). It is the layout that previously broke the download
// ("missing usable encoding/width/height" / empty data from BINARY readers).
//
// The fixture is ~31 MB and is NOT committed; the test SKIPS when it is absent
// (CI), and runs locally after `toolbox_mosaico_live_harness` regenerates it.

#include <arrow/api.h>
#include <arrow/array/array_binary.h>
#include <arrow/io/file.h>
#include <arrow/ipc/reader.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../src/arrow_ingest.hpp"
#include "../src/image_metadata.hpp"
#include "pj_base/builtin/image_codec.hpp"

namespace {

// Recording fake toolbox host: captures registerObjectTopic + pushOwnedObject
// so the test can inspect the topic metadata and every serialized blob.
struct RecordedObjectTopic {
  std::string name;
  std::string metadata_json;
};
struct RecordedPush {
  std::int64_t ts_ns = 0;
  std::vector<std::uint8_t> payload;
};

struct FakeHost {
  std::vector<std::string> data_sources;
  std::vector<RecordedObjectTopic> object_topics;
  std::vector<RecordedPush> pushes;
  std::uint32_t next_id = 1;
  std::mutex mu;

  PJ::sdk::ToolboxHostView view() {
    PJ_toolbox_host_t host{};
    host.ctx = this;
    host.vtable = &kVtable;
    return PJ::sdk::ToolboxHostView(host);
  }

  static FakeHost* self(void* ctx) {
    return static_cast<FakeHost*>(ctx);
  }
  static std::string toStr(PJ_string_view_t s) {
    return (s.data != nullptr && s.size > 0) ? std::string(s.data, s.size) : std::string();
  }
  static bool createDataSource(void* ctx, PJ_string_view_t name, PJ_data_source_handle_t* out, PJ_error_t*)
      PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    h->data_sources.push_back(toStr(name));
    out->id = h->next_id++;
    return true;
  }
  static bool registerObjectTopic(
      void* ctx, PJ_data_source_handle_t, PJ_string_view_t topic_name, PJ_string_view_t metadata_json,
      PJ_object_topic_handle_t* out, PJ_error_t*) PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    h->object_topics.push_back({toStr(topic_name), toStr(metadata_json)});
    out->id = h->next_id++;
    return true;
  }
  static bool pushOwnedObject(
      void* ctx, PJ_object_topic_handle_t, int64_t ts, const uint8_t* data, size_t size, PJ_error_t*) PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    RecordedPush rec;
    rec.ts_ns = ts;
    if (data != nullptr && size > 0) {
      rec.payload.assign(data, data + size);
    }
    h->pushes.push_back(std::move(rec));
    return true;
  }

  static const PJ_toolbox_host_vtable_t kVtable;
};

const PJ_toolbox_host_vtable_t FakeHost::kVtable = [] {
  PJ_toolbox_host_vtable_t v{};
  // struct_size MUST be set: ToolboxHostView::hasTailSlot gates object-topic
  // support on `struct_size >= offsetof(slot) + sizeof(fn)`.
  v.abi_version = 0;
  v.struct_size = sizeof(PJ_toolbox_host_vtable_t);
  v.create_data_source = &FakeHost::createDataSource;
  v.register_object_topic = &FakeHost::registerObjectTopic;
  v.push_owned_object = &FakeHost::pushOwnedObject;
  return v;
}();

// Locate the captured fixture relative to this source file's tests/ dir.
std::string fixturePath() {
  std::filesystem::path here(__FILE__);
  return (here.parent_path() / "fixtures" / "bonirob_2016_04_20_16_31_15_21_camera_jai_rgb_image_batch.arrow").string();
}

std::shared_ptr<arrow::Table> loadFixtureTable(const std::string& path) {
  auto infile = arrow::io::ReadableFile::Open(path);
  if (!infile.ok()) {
    return nullptr;
  }
  auto reader = arrow::ipc::RecordBatchFileReader::Open(*infile);
  if (!reader.ok()) {
    return nullptr;
  }
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  for (int i = 0; i < (*reader)->num_record_batches(); ++i) {
    auto b = (*reader)->ReadRecordBatch(i);
    if (!b.ok()) {
      return nullptr;
    }
    batches.push_back(*b);
  }
  if (batches.empty()) {
    return nullptr;
  }
  auto table = arrow::Table::FromRecordBatches(batches);
  return table.ok() ? *table : nullptr;
}

bool hasPngSignature(const std::vector<std::uint8_t>& b) {
  static constexpr std::array<std::uint8_t, 8> kPng{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  return b.size() >= kPng.size() && std::equal(kPng.begin(), kPng.end(), b.begin());
}

}  // namespace

TEST(MosaicoImageRealFixture, RealServerBatchSerializesToValidPerFrameImages) {
  const std::string path = fixturePath();
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "fixture absent (regenerate via toolbox_mosaico_live_harness): " << path;
  }
  auto table = loadFixtureTable(path);
  ASSERT_NE(table, nullptr) << "failed to load Arrow IPC fixture: " << path;
  ASSERT_GT(table->num_rows(), 0);

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("bonirob_2016-04-20-16-31-15_21");
  ASSERT_TRUE(ds.has_value());

  auto outcome = mosaico::pushImageRowsToHost(
      host, *ds, "/camera/jai/rgb/image", table, "timestamp_ns", /*synth_anchor_ns=*/0,
      /*synth_interval_ns=*/0);
  ASSERT_TRUE(outcome.has_value()) << "pushImageRowsToHost failed: " << outcome.error();
  EXPECT_EQ(outcome->skipped, 0) << "first skip reason: " << outcome->first_error;
  EXPECT_EQ(outcome->pushed, table->num_rows());

  // Topic registered exactly once with the canonical metadata.
  ASSERT_EQ(fake.object_topics.size(), 1u);
  EXPECT_EQ(fake.object_topics[0].name, "/camera/jai/rgb/image");
  EXPECT_EQ(fake.object_topics[0].metadata_json, std::string(mosaico::kCanonicalImageMetadata));

  // One blob per row, and each blob round-trips to a valid PNG-wrapped Bayer frame.
  ASSERT_EQ(fake.pushes.size(), static_cast<std::size_t>(table->num_rows()));
  for (std::size_t i = 0; i < fake.pushes.size(); ++i) {
    const auto& push = fake.pushes[i];
    ASSERT_FALSE(push.payload.empty()) << "row " << i << " pushed empty payload";
    auto img = PJ::deserializeImage(push.payload.data(), push.payload.size());
    ASSERT_TRUE(img.has_value()) << "row " << i << " deserialize failed: " << img.error();
    EXPECT_EQ(img->width, 1296u) << "row " << i;
    EXPECT_EQ(img->height, 966u) << "row " << i;
    EXPECT_EQ(img->encoding, "bayer_rggb8") << "row " << i;
    EXPECT_EQ(img->row_step, 1296u) << "row " << i;
    EXPECT_FALSE(img->is_bigendian) << "row " << i;
    EXPECT_FALSE(img->data.empty()) << "row " << i;
    EXPECT_TRUE(hasPngSignature(std::vector<std::uint8_t>(img->data.begin(), img->data.end())))
        << "row " << i << " image data is not a PNG container as expected for this topic";
    EXPECT_GT(push.ts_ns, 0) << "row " << i << " has no per-row timestamp";
  }
}
