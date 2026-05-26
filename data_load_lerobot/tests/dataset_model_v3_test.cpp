// End-to-end test for LeRobot v3.0 dataset loading.
//
// Builds a minimal but real v3.0 dataset on disk (info.json + meta/episodes
// chunked parquet + data chunked parquet + an empty mp4 placeholder) using
// Arrow's writer, then loads it via `loadDatasetModel` and asserts that
// `model.version == V3_0`, `episodes` and `episode_shards` are populated
// with the expected boundaries, and `episodeParquet/Video` route to the
// consolidated shard paths (not to per-episode files like v2.x).
//
// This is the v3.0 equivalent of `dataset_model_test.cpp::ParsesInfoEpisodesAndTasks`.
// Uses Arrow because v3.0 metadata is itself Parquet, so building the
// fixture with raw bytes isn't realistic — and the v3.0 reader exercises
// the same Arrow path the production importer uses against HF Hub datasets.

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "dataset_model.hpp"

namespace fs = std::filesystem;
using namespace lerobot;  // NOLINT(build/namespaces) — test-local convenience

namespace {

// Single source of truth for the fake episode boundaries used by the
// fixture. The math also feeds the `videos/.../from_timestamp` /
// `to_timestamp` doubles (length / fps).
constexpr double kFps = 10.0;
constexpr int64_t kEp0Len = 5;
constexpr int64_t kEp1Len = 7;
// Episode 0 occupies rows [0, 5), episode 1 [5, 12).
constexpr int64_t kEp0From = 0;
constexpr int64_t kEp0To = kEp0Len;
constexpr int64_t kEp1From = kEp0To;
constexpr int64_t kEp1To = kEp0To + kEp1Len;

// Writes an Arrow table to <path> as a Parquet file. Crashes (via ASSERT_*)
// on any Arrow error — fixture builders shouldn't recover.
void writeParquet(const std::shared_ptr<arrow::Table>& table, const fs::path& path) {
  auto outfile_or = arrow::io::FileOutputStream::Open(path.string());
  ASSERT_TRUE(outfile_or.ok()) << outfile_or.status().message();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *outfile_or, /*chunk_size=*/65536).ok());
}

// Build the per-episode shard parquet (`meta/episodes/chunk-000/file-000.parquet`).
// Columns mirror what we observed in lerobot/example_hil_serl_dataset; we
// keep only the ones the reader actually consumes (stats/* are skipped).
std::shared_ptr<arrow::Table> buildEpisodesShardTable() {
  arrow::Int64Builder ep_idx;
  EXPECT_TRUE(ep_idx.AppendValues({0, 1}).ok());

  arrow::Int64Builder length;
  EXPECT_TRUE(length.AppendValues({kEp0Len, kEp1Len}).ok());

  arrow::Int64Builder data_chunk;
  EXPECT_TRUE(data_chunk.AppendValues({0, 0}).ok());
  arrow::Int64Builder data_file;
  EXPECT_TRUE(data_file.AppendValues({0, 0}).ok());

  arrow::Int64Builder ds_from;
  EXPECT_TRUE(ds_from.AppendValues({kEp0From, kEp1From}).ok());
  arrow::Int64Builder ds_to;
  EXPECT_TRUE(ds_to.AppendValues({kEp0To, kEp1To}).ok());

  arrow::Int64Builder vid_chunk;
  EXPECT_TRUE(vid_chunk.AppendValues({0, 0}).ok());
  arrow::Int64Builder vid_file;
  EXPECT_TRUE(vid_file.AppendValues({0, 0}).ok());

  arrow::DoubleBuilder vid_from;
  EXPECT_TRUE(vid_from.AppendValues({kEp0From / kFps, kEp1From / kFps}).ok());
  arrow::DoubleBuilder vid_to;
  EXPECT_TRUE(vid_to.AppendValues({kEp0To / kFps, kEp1To / kFps}).ok());

  // tasks: list<string>, one task per episode
  auto tasks_inner = std::make_shared<arrow::StringBuilder>();
  arrow::ListBuilder tasks(arrow::default_memory_pool(), tasks_inner);
  EXPECT_TRUE(tasks.Append().ok());
  EXPECT_TRUE(tasks_inner->Append("grab the cube").ok());
  EXPECT_TRUE(tasks.Append().ok());
  EXPECT_TRUE(tasks_inner->Append("drop the cube").ok());

  std::shared_ptr<arrow::Array> ep_idx_arr;
  EXPECT_TRUE(ep_idx.Finish(&ep_idx_arr).ok());
  std::shared_ptr<arrow::Array> length_arr;
  EXPECT_TRUE(length.Finish(&length_arr).ok());
  std::shared_ptr<arrow::Array> data_chunk_arr;
  EXPECT_TRUE(data_chunk.Finish(&data_chunk_arr).ok());
  std::shared_ptr<arrow::Array> data_file_arr;
  EXPECT_TRUE(data_file.Finish(&data_file_arr).ok());
  std::shared_ptr<arrow::Array> ds_from_arr;
  EXPECT_TRUE(ds_from.Finish(&ds_from_arr).ok());
  std::shared_ptr<arrow::Array> ds_to_arr;
  EXPECT_TRUE(ds_to.Finish(&ds_to_arr).ok());
  std::shared_ptr<arrow::Array> vid_chunk_arr;
  EXPECT_TRUE(vid_chunk.Finish(&vid_chunk_arr).ok());
  std::shared_ptr<arrow::Array> vid_file_arr;
  EXPECT_TRUE(vid_file.Finish(&vid_file_arr).ok());
  std::shared_ptr<arrow::Array> vid_from_arr;
  EXPECT_TRUE(vid_from.Finish(&vid_from_arr).ok());
  std::shared_ptr<arrow::Array> vid_to_arr;
  EXPECT_TRUE(vid_to.Finish(&vid_to_arr).ok());
  std::shared_ptr<arrow::Array> tasks_arr;
  EXPECT_TRUE(tasks.Finish(&tasks_arr).ok());

  auto schema = arrow::schema({
      arrow::field("episode_index", arrow::int64()),
      arrow::field("length", arrow::int64()),
      arrow::field("tasks", arrow::list(arrow::utf8())),
      arrow::field("data/chunk_index", arrow::int64()),
      arrow::field("data/file_index", arrow::int64()),
      arrow::field("dataset_from_index", arrow::int64()),
      arrow::field("dataset_to_index", arrow::int64()),
      arrow::field("videos/observation.images.top/chunk_index", arrow::int64()),
      arrow::field("videos/observation.images.top/file_index", arrow::int64()),
      arrow::field("videos/observation.images.top/from_timestamp", arrow::float64()),
      arrow::field("videos/observation.images.top/to_timestamp", arrow::float64()),
  });
  return arrow::Table::Make(
      schema, {ep_idx_arr, length_arr, tasks_arr, data_chunk_arr, data_file_arr, ds_from_arr, ds_to_arr, vid_chunk_arr,
               vid_file_arr, vid_from_arr, vid_to_arr});
}

// Build the consolidated data parquet (`data/chunk-000/file-000.parquet`).
// Two episodes concatenated: ep 0 (5 rows) + ep 1 (7 rows) = 12 rows total.
std::shared_ptr<arrow::Table> buildDataShardTable() {
  arrow::Int64Builder ep_idx, frame_idx, idx;
  arrow::DoubleBuilder ts;
  for (int64_t i = 0; i < kEp0Len; ++i) {
    EXPECT_TRUE(ep_idx.Append(0).ok());
    EXPECT_TRUE(frame_idx.Append(i).ok());
    EXPECT_TRUE(idx.Append(i).ok());
    EXPECT_TRUE(ts.Append(static_cast<double>(i) / kFps).ok());
  }
  for (int64_t i = 0; i < kEp1Len; ++i) {
    EXPECT_TRUE(ep_idx.Append(1).ok());
    EXPECT_TRUE(frame_idx.Append(i).ok());
    EXPECT_TRUE(idx.Append(kEp0Len + i).ok());
    EXPECT_TRUE(ts.Append(static_cast<double>(kEp0Len + i) / kFps).ok());
  }
  std::shared_ptr<arrow::Array> ep_idx_arr;
  EXPECT_TRUE(ep_idx.Finish(&ep_idx_arr).ok());
  std::shared_ptr<arrow::Array> frame_idx_arr;
  EXPECT_TRUE(frame_idx.Finish(&frame_idx_arr).ok());
  std::shared_ptr<arrow::Array> idx_arr;
  EXPECT_TRUE(idx.Finish(&idx_arr).ok());
  std::shared_ptr<arrow::Array> ts_arr;
  EXPECT_TRUE(ts.Finish(&ts_arr).ok());

  auto schema = arrow::schema({
      arrow::field("episode_index", arrow::int64()),
      arrow::field("frame_index", arrow::int64()),
      arrow::field("index", arrow::int64()),
      arrow::field("timestamp", arrow::float64()),
  });
  return arrow::Table::Make(schema, {ep_idx_arr, frame_idx_arr, idx_arr, ts_arr});
}

// Builds a minimal v3.0 dataset on disk under a fresh temp dir.
fs::path makeV3Fixture() {
  const fs::path root =
      fs::temp_directory_path() /
      fs::path(std::string("lerobot_v3_fixture_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
  fs::remove_all(root);
  fs::create_directories(root / "meta" / "episodes" / "chunk-000");
  fs::create_directories(root / "data" / "chunk-000");
  fs::create_directories(root / "videos" / "observation.images.top" / "chunk-000");

  std::ofstream(root / "meta" / "info.json") << R"({
    "codebase_version": "v3.0",
    "robot_type": null,
    "total_episodes": 2,
    "total_frames": 12,
    "fps": 10,
    "chunks_size": 1000,
    "data_path": "data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet",
    "video_path": "videos/{video_key}/chunk-{chunk_index:03d}/file-{file_index:03d}.mp4",
    "features": {
      "observation.state": {"dtype": "float32", "shape": [3], "names": ["a","b","c"]},
      "action": {"dtype": "float32", "shape": [2]},
      "observation.images.top": {"dtype": "video", "shape": [3,128,128],
        "info": {"video.fps": 10, "video.codec": "av1"}}
    }
  })";

  // Stub mp4: the model layer never opens the bytes, only stores the path.
  std::ofstream(root / "videos" / "observation.images.top" / "chunk-000" / "file-000.mp4") << "FAKE";

  writeParquet(buildEpisodesShardTable(), root / "meta" / "episodes" / "chunk-000" / "file-000.parquet");
  writeParquet(buildDataShardTable(), root / "data" / "chunk-000" / "file-000.parquet");
  return root;
}

}  // namespace

TEST(LoadDatasetModelV3, AcceptsCodebaseVersionAndPopulatesShards) {
  const fs::path root = makeV3Fixture();
  ASSERT_FALSE(::testing::Test::HasFatalFailure());

  auto model_or = loadDatasetModel(root / "meta" / "info.json");
  ASSERT_TRUE(model_or.has_value()) << (model_or.has_value() ? "" : model_or.error());
  const DatasetModel& model = *model_or;

  EXPECT_EQ(model.codebase_version, "v3.0");
  EXPECT_EQ(model.version, DatasetVersion::V3_0);
  EXPECT_DOUBLE_EQ(model.fps, 10.0);

  ASSERT_EQ(model.episodes.size(), 2u);
  EXPECT_EQ(model.episodes[0].episode_index, 0);
  EXPECT_EQ(model.episodes[0].length, kEp0Len);
  EXPECT_EQ(model.episodes[0].task_text, "grab the cube");
  EXPECT_EQ(model.episodes[1].episode_index, 1);
  EXPECT_EQ(model.episodes[1].length, kEp1Len);
  EXPECT_EQ(model.episodes[1].task_text, "drop the cube");

  ASSERT_EQ(model.episode_shards.size(), 2u);
  const EpisodeShard& s0 = model.episode_shards.at(0);
  EXPECT_EQ(s0.row_from, kEp0From);
  EXPECT_EQ(s0.row_to, kEp0To);
  EXPECT_EQ(s0.parquet_path, root / "data" / "chunk-000" / "file-000.parquet");
  ASSERT_TRUE(s0.videos.count("observation.images.top") > 0);
  const VideoShard& v0 = s0.videos.at("observation.images.top");
  EXPECT_EQ(v0.mp4_path, root / "videos" / "observation.images.top" / "chunk-000" / "file-000.mp4");
  // from_timestamp 0.0s, to_timestamp 0.5s → 0 ns and 500_000_000 ns.
  ASSERT_TRUE(v0.start_ns.has_value());
  EXPECT_EQ(*v0.start_ns, 0);
  ASSERT_TRUE(v0.end_ns.has_value());
  EXPECT_EQ(*v0.end_ns, 500'000'000LL);

  const EpisodeShard& s1 = model.episode_shards.at(1);
  EXPECT_EQ(s1.row_from, kEp1From);
  EXPECT_EQ(s1.row_to, kEp1To);
  const VideoShard& v1 = s1.videos.at("observation.images.top");
  ASSERT_TRUE(v1.start_ns.has_value());
  EXPECT_EQ(*v1.start_ns, 500'000'000LL);  // 0.5s
  ASSERT_TRUE(v1.end_ns.has_value());
  EXPECT_EQ(*v1.end_ns, 1'200'000'000LL);  // 1.2s

  // episodeParquet/Video now resolve via episode_shards, not templates.
  EXPECT_EQ(model.episodeParquet(0), s0.parquet_path);
  EXPECT_EQ(model.episodeParquet(1), s1.parquet_path);
  EXPECT_EQ(model.episodeVideo(0, "observation.images.top"), v0.mp4_path);

  fs::remove_all(root);
}

TEST(LoadDatasetModelV3, PathTemplateExpandsChunkAndFileIndex) {
  // Sanity check that the generic expandPathTemplate honours v3.0's
  // {chunk_index} / {file_index} placeholders (the schema in info.json).
  const std::string tmpl = "data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet";
  const std::string out = expandPathTemplate(tmpl, {{"chunk_index", 7}, {"file_index", 42}});
  EXPECT_EQ(out, "data/chunk-007/file-042.parquet");
}
