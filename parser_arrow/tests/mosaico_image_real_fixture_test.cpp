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

#include "mosaico_object_capture.hpp"
#include "pj_base/builtin/image_codec.hpp"

namespace {

// Recording fake toolbox host: captures registerObjectTopic + pushOwnedObject
// so the test can inspect the topic metadata and every serialized blob.

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

  ObjectCapture fake;

  auto outcome = fake.parse("image", {"timestamp_ns", /*synth_anchor_ns=*/0, /*synth_interval_ns=*/0}, table);
  ASSERT_TRUE(outcome.has_value()) << "pushImageRowsToHost failed: " << outcome.error();
  EXPECT_EQ(outcome->skipped, 0) << "first skip reason: " << outcome->first_error;
  EXPECT_EQ(outcome->pushed, table->num_rows());

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
