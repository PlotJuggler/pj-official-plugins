// v3.0 episode-shard reader — Arrow + Parquet implementation.
//
// Reads `meta/episodes/chunk-XXX/file-NNN.parquet` shards and, for each row,
// populates one `EpisodeShard` (data parquet path + row range + per-camera
// video paths and timestamp windows) and one `EpisodeInfo` (for the dialog
// list) on the `DatasetModel`.
//
// Column names in the v3.0 episodes parquet use slash as separator
// (`data/chunk_index`, `videos/{camera}/from_timestamp`, …) — verified
// against `lerobot/example_hil_serl_dataset` on the Hub. The reader looks up
// each column by exact name; missing required columns surface as an error.

#include "dataset_shards_v3.hpp"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include "dataset_model.hpp"

namespace lerobot {
namespace {

namespace fs = std::filesystem;

/// Locate a column by exact name. Returns nullptr if not found.
std::shared_ptr<arrow::ChunkedArray> column(const arrow::Table& t, const std::string& name) {
  const int idx = t.schema()->GetFieldIndex(name);
  if (idx < 0) {
    return nullptr;
  }
  return t.column(idx);
}

/// Required column: error if missing.
PJ::Expected<std::shared_ptr<arrow::ChunkedArray>> requireColumn(const arrow::Table& t, const std::string& name) {
  auto c = column(t, name);
  if (c == nullptr) {
    return PJ::unexpected("v3.0 episode shard missing required column: " + name);
  }
  return c;
}

/// Required column with a declared Arrow type. Surfaces a clear error if the
/// shard ever emits the column with a different physical type (e.g. int32
/// instead of int64) — otherwise `static_pointer_cast` below would return 0
/// silently and the importer would skip the episode without diagnostic.
PJ::Expected<std::shared_ptr<arrow::ChunkedArray>> requireTypedColumn(
    const arrow::Table& t, const std::string& name, arrow::Type::type expected) {
  auto c_or = requireColumn(t, name);
  if (!c_or) {
    return c_or;
  }
  if ((*c_or)->type()->id() != expected) {
    return PJ::unexpected(
        "v3.0 episode shard column '" + name + "' has type " + (*c_or)->type()->ToString() +
        " (expected Arrow type id " + std::to_string(static_cast<int>(expected)) + ")");
  }
  return c_or;
}

/// Extract one int64 value at row `i` from a chunked int64 column.
int64_t int64At(const arrow::ChunkedArray& col, int64_t i) {
  for (const auto& chunk : col.chunks()) {
    const int64_t n = chunk->length();
    if (i < n) {
      return std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(i);
    }
    i -= n;
  }
  return 0;
}

/// Extract one double value at row `i` from a chunked double column.
double doubleAt(const arrow::ChunkedArray& col, int64_t i) {
  for (const auto& chunk : col.chunks()) {
    const int64_t n = chunk->length();
    if (i < n) {
      return std::static_pointer_cast<arrow::DoubleArray>(chunk)->Value(i);
    }
    i -= n;
  }
  return 0.0;
}

/// Extract one list<string> value at row `i` from a chunked list column.
std::vector<std::string> stringListAt(const arrow::ChunkedArray& col, int64_t i) {
  std::vector<std::string> out;
  for (const auto& chunk : col.chunks()) {
    const int64_t n = chunk->length();
    if (i < n) {
      const auto& list = static_cast<const arrow::ListArray&>(*chunk);
      if (list.IsNull(i)) {
        return out;
      }
      const auto values = std::static_pointer_cast<arrow::StringArray>(list.values());
      const int64_t off = list.value_offset(i);
      const int64_t len = list.value_length(i);
      out.reserve(static_cast<std::size_t>(len));
      for (int64_t k = 0; k < len; ++k) {
        out.emplace_back(values->GetString(off + k));
      }
      return out;
    }
    i -= n;
  }
  return out;
}

/// Read a single `meta/episodes/.../*.parquet` shard into an Arrow table.
PJ::Expected<std::shared_ptr<arrow::Table>> readShardTable(const fs::path& shard_path) {
  auto fs_or = arrow::io::ReadableFile::Open(shard_path.string());
  if (!fs_or.ok()) {
    return PJ::unexpected("cannot open " + shard_path.string() + ": " + fs_or.status().message());
  }
  auto reader_or = parquet::arrow::OpenFile(*fs_or, arrow::default_memory_pool());
  if (!reader_or.ok()) {
    return PJ::unexpected("cannot read parquet " + shard_path.string() + ": " + reader_or.status().message());
  }
  std::unique_ptr<parquet::arrow::FileReader> reader = std::move(*reader_or);
  std::shared_ptr<arrow::Table> table;
  if (auto st = reader->ReadTable(&table); !st.ok()) {
    return PJ::unexpected("cannot read table " + shard_path.string() + ": " + st.message());
  }
  return table;
}

/// Walk `root/meta/episodes/` and return the shard parquets in (chunk, file)
/// order. Empty list means there is no `meta/episodes/` directory.
std::vector<fs::path> findEpisodeShards(const fs::path& root) {
  std::vector<fs::path> out;
  const fs::path episodes_dir = root / "meta" / "episodes";
  std::error_code ec;
  if (!fs::is_directory(episodes_dir, ec)) {
    return out;
  }
  for (const auto& chunk_entry : fs::directory_iterator(episodes_dir, ec)) {
    if (!chunk_entry.is_directory()) {
      continue;
    }
    for (const auto& file_entry : fs::directory_iterator(chunk_entry, ec)) {
      if (!file_entry.is_regular_file()) {
        continue;
      }
      if (file_entry.path().extension() == ".parquet") {
        out.push_back(file_entry.path());
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

PJ::Status loadV3EpisodeShards(const fs::path& root, DatasetModel& model) {
  const std::vector<fs::path> shards = findEpisodeShards(root);
  if (shards.empty()) {
    return PJ::unexpected("v3.0 dataset missing meta/episodes/*.parquet under " + root.string());
  }

  for (const fs::path& shard_path : shards) {
    auto table_or = readShardTable(shard_path);
    if (!table_or) {
      return PJ::unexpected(table_or.error());
    }
    const arrow::Table& table = **table_or;

    auto ep_col_or = requireTypedColumn(table, "episode_index", arrow::Type::INT64);
    if (!ep_col_or) {
      return PJ::unexpected(ep_col_or.error());
    }
    auto len_col_or = requireTypedColumn(table, "length", arrow::Type::INT64);
    if (!len_col_or) {
      return PJ::unexpected(len_col_or.error());
    }
    auto data_chunk_col_or = requireTypedColumn(table, "data/chunk_index", arrow::Type::INT64);
    if (!data_chunk_col_or) {
      return PJ::unexpected(data_chunk_col_or.error());
    }
    auto data_file_col_or = requireTypedColumn(table, "data/file_index", arrow::Type::INT64);
    if (!data_file_col_or) {
      return PJ::unexpected(data_file_col_or.error());
    }
    auto from_idx_col_or = requireTypedColumn(table, "dataset_from_index", arrow::Type::INT64);
    if (!from_idx_col_or) {
      return PJ::unexpected(from_idx_col_or.error());
    }
    auto to_idx_col_or = requireTypedColumn(table, "dataset_to_index", arrow::Type::INT64);
    if (!to_idx_col_or) {
      return PJ::unexpected(to_idx_col_or.error());
    }
    const auto tasks_col = column(table, "tasks");  // optional

    // Pre-resolve per-camera column tuples once per shard. All four columns
    // must be present for the camera to be usable; if any is missing the
    // shard is malformed and we surface a clear error rather than silently
    // dropping a declared camera (which would leave the user wondering why
    // their feature does not appear in the catalog).
    struct CamCols {
      std::shared_ptr<arrow::ChunkedArray> chunk_index;
      std::shared_ptr<arrow::ChunkedArray> file_index;
      std::shared_ptr<arrow::ChunkedArray> from_ts;
      std::shared_ptr<arrow::ChunkedArray> to_ts;
    };
    std::vector<std::pair<std::string, CamCols>> cam_cols;
    cam_cols.reserve(model.camera_names.size());
    for (const std::string& cam : model.camera_names) {
      const std::string prefix = "videos/" + cam + "/";
      auto ci = requireTypedColumn(table, prefix + "chunk_index", arrow::Type::INT64);
      auto fi = requireTypedColumn(table, prefix + "file_index", arrow::Type::INT64);
      auto ft = requireTypedColumn(table, prefix + "from_timestamp", arrow::Type::DOUBLE);
      auto tt = requireTypedColumn(table, prefix + "to_timestamp", arrow::Type::DOUBLE);
      if (!ci || !fi || !ft || !tt) {
        return PJ::unexpected(
            "v3.0 episode shard has incomplete columns for camera '" + cam + "': " +
            (!ci   ? ci.error()
             : !fi ? fi.error()
             : !ft ? ft.error()
                   : tt.error()));
      }
      cam_cols.emplace_back(cam, CamCols{*ci, *fi, *ft, *tt});
    }

    const int64_t num_rows = table.num_rows();
    for (int64_t i = 0; i < num_rows; ++i) {
      const int64_t ep_idx = int64At(*ep_col_or.value(), i);
      const int64_t length = int64At(*len_col_or.value(), i);
      const int64_t data_chunk = int64At(*data_chunk_col_or.value(), i);
      const int64_t data_file = int64At(*data_file_col_or.value(), i);
      const int64_t row_from = int64At(*from_idx_col_or.value(), i);
      const int64_t row_to = int64At(*to_idx_col_or.value(), i);

      EpisodeShard shard;
      shard.parquet_path =
          model.root /
          expandPathTemplate(model.data_path_tmpl, {{"chunk_index", data_chunk}, {"file_index", data_file}});
      shard.row_from = row_from;
      shard.row_to = row_to;
      if (tasks_col != nullptr) {
        shard.tasks = stringListAt(*tasks_col, i);
      }

      for (const auto& [cam, cc] : cam_cols) {
        VideoShard vs;
        const int64_t v_chunk = int64At(*cc.chunk_index, i);
        const int64_t v_file = int64At(*cc.file_index, i);
        const double from_s = doubleAt(*cc.from_ts, i);
        const double to_s = doubleAt(*cc.to_ts, i);
        vs.mp4_path = model.root / expandPathTemplate(
                                       model.video_path_tmpl, {{"chunk_index", v_chunk}, {"file_index", v_file}},
                                       {{"video_key", cam}, {"camera_key", cam}});
        vs.start_ns = static_cast<int64_t>(std::llround(from_s * 1e9));
        vs.end_ns = static_cast<int64_t>(std::llround(to_s * 1e9));
        shard.videos[cam] = std::move(vs);
      }

      EpisodeInfo info;
      info.episode_index = ep_idx;
      info.length = length;
      info.task_text = shard.tasks.empty() ? "" : shard.tasks.front();
      model.episodes.push_back(std::move(info));
      model.episode_shards[ep_idx] = std::move(shard);
    }
  }

  std::sort(model.episodes.begin(), model.episodes.end(), [](const EpisodeInfo& a, const EpisodeInfo& b) {
    return a.episode_index < b.episode_index;
  });
  return PJ::okStatus();
}

}  // namespace lerobot
