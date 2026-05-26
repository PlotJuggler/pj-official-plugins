#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <pj_base/builtin/asset_video.hpp>
#include <pj_base/builtin/asset_video_codec.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/media_metadata.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "dataset_model.hpp"
#include "flatten_plan.hpp"
#include "lerobot_dialog.hpp"
#include "lerobot_manifest.hpp"
#include "pj_arrow_helpers/arrow_helpers.hpp"

namespace {

using pj::arrow_helpers::arrowTypeToPrimitive;
using pj::arrow_helpers::getArrowValueRef;
using pj::arrow_helpers::isSupportedArrowType;

// Per-row ns timestamp on the episode's 0-based clock. Uses the parquet
// `timestamp` column when present, otherwise `frame_index / fps`. Each
// episode is its own DatasetId so no cross-episode offset arithmetic is
// needed.
int64_t rowTimestampNs(bool has_ts, double ts_seconds, int64_t frame_index, double fps) {
  if (has_ts) {
    return static_cast<int64_t>(std::llround(ts_seconds * 1e9));
  }
  const double safe_fps = fps > 0.0 ? fps : 1.0;
  return static_cast<int64_t>(std::llround(static_cast<double>(frame_index) / safe_fps * 1e9));
}

// Guard against an absurd `shape` in info.json blowing up into millions of
// series (a vector feature in real datasets is a handful of elements).
constexpr int kMaxVectorWidth = 4096;

struct OutColumn {
  std::string out_name;    // series name (stable, owns the string)
  std::string arrow_name;  // source parquet column
  int vec_k = -1;          // -1 = scalar column; >=0 = element of a list column
  arrow::Type::type scalar_type = arrow::Type::NA;
  PJ::PrimitiveType prim = PJ::PrimitiveType::kFloat64;
};

double readSeconds(const std::shared_ptr<arrow::Array>& a, int64_t row) {
  switch (a->type_id()) {
    case arrow::Type::FLOAT:
      return static_cast<double>(std::static_pointer_cast<arrow::FloatArray>(a)->Value(row));
    case arrow::Type::DOUBLE:
      return std::static_pointer_cast<arrow::DoubleArray>(a)->Value(row);
    case arrow::Type::INT32:
      return static_cast<double>(std::static_pointer_cast<arrow::Int32Array>(a)->Value(row));
    case arrow::Type::INT64:
      return static_cast<double>(std::static_pointer_cast<arrow::Int64Array>(a)->Value(row));
    default:
      return 0.0;
  }
}

int64_t readInt(const std::shared_ptr<arrow::Array>& a, int64_t row, int64_t fallback) {
  switch (a->type_id()) {
    case arrow::Type::INT32:
      return std::static_pointer_cast<arrow::Int32Array>(a)->Value(row);
    case arrow::Type::INT64:
      return std::static_pointer_cast<arrow::Int64Array>(a)->Value(row);
    case arrow::Type::UINT32:
      return static_cast<int64_t>(std::static_pointer_cast<arrow::UInt32Array>(a)->Value(row));
    case arrow::Type::UINT64:
      return static_cast<int64_t>(std::static_pointer_cast<arrow::UInt64Array>(a)->Value(row));
    default:
      return fallback;
  }
}

// True for `list<float|double>` / `fixed_size_list<float|double>` columns
// (LeRobot observation.state / action are stored this way).
bool isFloatVectorColumn(const std::shared_ptr<arrow::DataType>& type) {
  std::shared_ptr<arrow::DataType> value_type;
  if (type->id() == arrow::Type::FIXED_SIZE_LIST) {
    value_type = std::static_pointer_cast<arrow::FixedSizeListType>(type)->value_type();
  } else if (type->id() == arrow::Type::LIST) {
    value_type = std::static_pointer_cast<arrow::ListType>(type)->value_type();
  } else if (type->id() == arrow::Type::LARGE_LIST) {
    value_type = std::static_pointer_cast<arrow::LargeListType>(type)->value_type();
  } else {
    return false;
  }
  return value_type->id() == arrow::Type::FLOAT || value_type->id() == arrow::Type::DOUBLE;
}

// A vector-column cell viewed as its flat child float values for one row.
struct FloatVectorCell {
  std::shared_ptr<arrow::DoubleArray> d_values;  // set if child is double
  std::shared_ptr<arrow::FloatArray> f_values;   // set if child is float
  int64_t offset = 0;                            // start index into the child array for this row
  int64_t width = 0;                             // number of elements for this row

  [[nodiscard]] bool isNull(int64_t k) const {
    if (d_values) {
      return d_values->IsNull(offset + k);
    }
    if (f_values) {
      return f_values->IsNull(offset + k);
    }
    return true;
  }
  [[nodiscard]] double value(int64_t k) const {
    if (d_values) {
      return d_values->Value(offset + k);
    }
    if (f_values) {
      return static_cast<double>(f_values->Value(offset + k));
    }
    return 0.0;
  }
};

// Resolve the child float values + [offset,width) for `row` of a
// list/fixed_size_list float column. width==0 if the cell is null/empty.
FloatVectorCell floatVectorCell(const std::shared_ptr<arrow::Array>& array, int64_t row) {
  FloatVectorCell cell;
  std::shared_ptr<arrow::Array> child;
  if (array->type_id() == arrow::Type::FIXED_SIZE_LIST) {
    auto fsl = std::static_pointer_cast<arrow::FixedSizeListArray>(array);
    if (fsl->IsNull(row)) {
      return cell;
    }
    const int64_t w = fsl->value_length();
    cell.offset = (fsl->offset() + row) * w;
    cell.width = w;
    child = fsl->values();
  } else if (array->type_id() == arrow::Type::LIST) {
    auto la = std::static_pointer_cast<arrow::ListArray>(array);
    if (la->IsNull(row)) {
      return cell;
    }
    cell.offset = la->value_offset(row);
    cell.width = la->value_length(row);
    child = la->values();
  } else if (array->type_id() == arrow::Type::LARGE_LIST) {
    auto la = std::static_pointer_cast<arrow::LargeListArray>(array);
    if (la->IsNull(row)) {
      return cell;
    }
    cell.offset = la->value_offset(row);
    cell.width = la->value_length(row);
    child = la->values();
  } else {
    return cell;
  }
  if (child->type_id() == arrow::Type::DOUBLE) {
    cell.d_values = std::static_pointer_cast<arrow::DoubleArray>(child);
  } else if (child->type_id() == arrow::Type::FLOAT) {
    cell.f_values = std::static_pointer_cast<arrow::FloatArray>(child);
  } else {
    cell.width = 0;
  }
  return cell;
}

/// LeRobot v2.1 dataset loader (numeric + per-camera video on one timeline).
class LeRobotSource : public PJ::FileSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected(std::string("invalid LeRobot config JSON"));
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    const lerobot::DatasetModel* model = dialog_.model();
    if (model == nullptr) {
      const std::string& err = dialog_.datasetError();
      return PJ::unexpected(err.empty() ? std::string("no LeRobot dataset loaded") : err);
    }

    // Each LeRobotSource instance imports exactly one episode into its own
    // DatasetId. FileLoader spawns N instances via `__pj_fanout` for an
    // N-episode selection; this code path therefore only ever sees a single
    // episode index in its config.
    const auto single_ep = dialog_.singleEpisode();
    if (!single_ep.has_value()) {
      return PJ::unexpected(std::string("LeRobot config missing required `episode` field"));
    }
    const int64_t ep = *single_ep;

    int64_t length = -1;
    for (const auto& e : model->episodes) {
      if (e.episode_index == ep) {
        length = e.length;
        break;
      }
    }
    if (length < 0) {
      return PJ::unexpected("selected episode " + std::to_string(ep) + " is not in the dataset");
    }

    auto topic = writeHost().ensureTopic("lerobot");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }

    const std::string path = model->episodeParquet(ep).string();
    auto opened = openParquetReader(path);
    if (!opened) {
      return PJ::unexpected(opened.error());
    }

    auto plan_or = buildPlan(*model, *opened->schema, path);
    if (!plan_or) {
      return PJ::unexpected(plan_or.error());
    }
    const std::vector<OutColumn>& plan = *plan_or;

    for (const auto& c : plan) {
      auto f = writeHost().ensureField(*topic, c.out_name, c.prim);
      if (!f) {
        return PJ::unexpected(f.error());
      }
    }

    (void)runtimeHost().progressStart("Importing LeRobot", static_cast<uint64_t>(length), true);

    int64_t processed = 0;
    auto st = importEpisode(*model, plan, *opened->reader, *opened->schema, path, *topic, processed);
    if (!st) {
      return st;
    }
    if (runtimeHost().isStopRequested()) {
      return PJ::unexpected(std::string("import cancelled"));
    }

    auto vst = importVideoAssets(*model, ep);
    if (!vst) {
      return vst;
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "LeRobot " + model->codebase_version + ": imported " + std::to_string(processed) + " rows + " +
            std::to_string(model->camera_names.size()) + " video asset(s) from episode " + std::to_string(ep));
    return PJ::okStatus();
  }

 private:
  struct OpenedParquet {
    std::unique_ptr<parquet::arrow::FileReader> reader;
    std::shared_ptr<arrow::Schema> schema;
  };

  static PJ::Expected<OpenedParquet> openParquetReader(const std::string& path) {
    auto infile = arrow::io::ReadableFile::Open(path);
    if (!infile.ok()) {
      return PJ::unexpected("cannot open parquet: " + path);
    }
    auto reader = parquet::arrow::OpenFile(*infile, arrow::default_memory_pool());
    if (!reader.ok()) {
      return PJ::unexpected("failed to open parquet: " + path);
    }
    OpenedParquet out;
    out.reader = std::move(*reader);
    if (!out.reader->GetSchema(&out.schema).ok()) {
      return PJ::unexpected("failed to read schema: " + path);
    }
    return out;
  }

  PJ::Expected<std::vector<OutColumn>> buildPlan(
      const lerobot::DatasetModel& model, const arrow::Schema& schema, const std::string& path) {
    std::vector<OutColumn> plan;
    std::vector<std::string> raw_names;  // for global dedupe
    for (int i = 0; i < schema.num_fields(); ++i) {
      const auto& field = schema.field(i);
      const std::string& name = field->name();
      const auto& type = field->type();
      if (name == "timestamp") {
        continue;  // synthesized into the time axis, not a series
      }
      if (isSupportedArrowType(type->id())) {
        OutColumn c;
        c.out_name = name;
        c.arrow_name = name;
        c.scalar_type = type->id();
        c.prim = arrowTypeToPrimitive(type->id());
        plan.push_back(std::move(c));
        raw_names.push_back(name);
      } else if (isFloatVectorColumn(type)) {
        int k = 0;
        if (type->id() == arrow::Type::FIXED_SIZE_LIST) {
          k = std::static_pointer_cast<arrow::FixedSizeListType>(type)->list_size();
        }
        const lerobot::FeatureSpec* fs = model.feature(name);
        if (k <= 0 && fs != nullptr && !fs->shape.empty()) {
          k = static_cast<int>(fs->shape.back());
        }
        if (k <= 0) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning, "skipping vector column '" + name + "' (unknown width)");
          continue;
        }
        if (k > kMaxVectorWidth) {
          return PJ::unexpected(
              "vector column '" + name + "' width " + std::to_string(k) + " exceeds limit " +
              std::to_string(kMaxVectorWidth));
        }
        const std::vector<std::string> labels =
            lerobot::flattenedFieldNames(name, k, fs != nullptr ? fs->names : std::vector<std::string>{});
        for (int e = 0; e < k; ++e) {
          OutColumn c;
          c.out_name = labels[static_cast<std::size_t>(e)];
          c.arrow_name = name;
          c.vec_k = e;
          c.prim = PJ::PrimitiveType::kFloat64;
          plan.push_back(std::move(c));
          raw_names.push_back(labels[static_cast<std::size_t>(e)]);
        }
      }
      // struct / other columns are intentionally skipped.
    }
    if (plan.empty()) {
      return PJ::unexpected("no supported columns in " + path);
    }
    const auto deduped = lerobot::dedupeFieldNames(raw_names);
    for (std::size_t i = 0; i < plan.size(); ++i) {
      plan[i].out_name = deduped[i];
    }
    return plan;
  }

  PJ::Status importEpisode(
      const lerobot::DatasetModel& model, const std::vector<OutColumn>& plan, parquet::arrow::FileReader& reader,
      const arrow::Schema& schema, const std::string& path, PJ::sdk::TopicHandle topic, int64_t& processed) {
    // Resolve column names → Arrow index once per episode (tolerating schema
    // drift across episodes); the row loop then indexes by position only.
    std::unordered_map<std::string, int> col_of;
    col_of.reserve(static_cast<std::size_t>(schema.num_fields()));
    for (int i = 0; i < schema.num_fields(); ++i) {
      col_of.emplace(schema.field(i)->name(), i);
    }
    auto idx_of = [&](const std::string& n) -> int {
      auto it = col_of.find(n);
      return it != col_of.end() ? it->second : -1;
    };
    const int ts_idx = idx_of("timestamp");
    const int frame_idx = idx_of("frame_index");
    std::vector<int> plan_idx(plan.size());
    for (std::size_t k = 0; k < plan.size(); ++k) {
      plan_idx[k] = idx_of(plan[k].arrow_name);
    }

    std::shared_ptr<arrow::RecordBatchReader> batches;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (!reader.GetRecordBatchReader(&batches).ok()) {
      return PJ::unexpected("failed to create batch reader: " + path);
    }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    // Resolve the row range to emit. v2.x → [0, infinity) (whole file is the
    // episode); v3.0 → [dataset_from_index, dataset_to_index) from the shard
    // map (one episode's slice inside a consolidated parquet that holds many).
    int64_t row_from = 0;
    int64_t row_to = std::numeric_limits<int64_t>::max();
    if (auto sit = model.episode_shards.find(ep); sit != model.episode_shards.end()) {
      row_from = sit->second.row_from;
      // A row_to of 0 means "shard never recorded an upper bound" — interpret
      // it as "until EOF" to stay backward-compatible with any shard map that
      // omits it (e.g. synthesized for an empty v2.x episode).
      row_to = sit->second.row_to > 0 ? sit->second.row_to : std::numeric_limits<int64_t>::max();
    }

    std::vector<PJ::sdk::NamedFieldValue> row_fields;
    row_fields.reserve(plan.size());
    std::vector<std::shared_ptr<arrow::Array>> cols(plan.size());
    std::shared_ptr<arrow::RecordBatch> batch;
    int64_t batch_start = 0;  // global row index of batch->column(*)[0]
    arrow::Status read_st;

    while ((read_st = batches->ReadNext(&batch)).ok() && batch) {
      const int64_t n = batch->num_rows();
      const int64_t batch_end = batch_start + n;
      // Trim the batch to [row_from, row_to). For v2.x with the default
      // row_from=0 and row_to=int64_max this collapses to [0, n) and the
      // loop is identical to the original whole-file iteration. For v3.0
      // the slice typically straddles or sits inside the batch.
      const int64_t r_first = std::max<int64_t>(0, row_from - batch_start);
      const int64_t r_last = std::min<int64_t>(n, row_to - batch_start);
      if (r_first >= r_last) {
        // Whole batch is outside the window — skip without touching arrays.
        if (batch_end >= row_to) {
          break;
        }
        batch_start = batch_end;
        continue;
      }
      std::shared_ptr<arrow::Array> ts_arr = ts_idx >= 0 ? batch->column(ts_idx) : nullptr;
      std::shared_ptr<arrow::Array> fidx_arr = frame_idx >= 0 ? batch->column(frame_idx) : nullptr;
      for (std::size_t k = 0; k < plan.size(); ++k) {
        cols[k] = plan_idx[k] >= 0 ? batch->column(plan_idx[k]) : nullptr;
      }

      for (int64_t r = r_first; r < r_last; ++r) {
        const bool has_ts = ts_arr != nullptr && !ts_arr->IsNull(r);
        const double sec = has_ts ? readSeconds(ts_arr, r) : 0.0;
        // Frame index inside the *episode*: relative to row_from so v3.0
        // episodes see frame_index starting at 0 just like v2.x, regardless
        // of where the slice sits inside the consolidated parquet.
        const int64_t episode_row = (batch_start + r) - row_from;
        const int64_t fi = fidx_arr != nullptr ? readInt(fidx_arr, r, episode_row) : episode_row;
        const int64_t ts_ns = rowTimestampNs(has_ts, sec, fi, model.fps);

        row_fields.clear();
        for (std::size_t k = 0; k < plan.size(); ++k) {
          const std::shared_ptr<arrow::Array>& arr = cols[k];
          if (!arr) {
            continue;
          }
          const OutColumn& c = plan[k];
          if (c.vec_k < 0) {
            auto v = getArrowValueRef(arr, r, c.scalar_type);
            if (!PJ::sdk::isNull(v)) {
              row_fields.push_back({.name = c.out_name, .value = v});
            }
          } else {
            const auto cell = floatVectorCell(arr, r);
            if (c.vec_k < cell.width && !cell.isNull(c.vec_k)) {
              row_fields.push_back({.name = c.out_name, .value = cell.value(c.vec_k)});
            }
          }
        }
        if (!row_fields.empty()) {
          auto st = writeHost().appendRecord(
              topic, PJ::Timestamp{ts_ns},
              PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields.data(), row_fields.size()));
          if (!st) {
            return st;
          }
        }
        ++processed;
      }
      batch_start = batch_end;
      (void)runtimeHost().progressUpdate(static_cast<uint64_t>(processed));
      if (batch_start >= row_to) {
        break;
      }
      if (runtimeHost().isStopRequested()) {
        return PJ::unexpected(std::string("import cancelled"));
      }
    }
    if (!read_st.ok()) {
      return PJ::unexpected("error reading parquet batches from " + path + ": " + read_st.ToString());
    }
    return PJ::okStatus();
  }

  PJ::Status importVideoAssets(const lerobot::DatasetModel& model, int64_t ep) {
    // Each camera gets ONE sdk::AssetVideo entry pointing at the episode's MP4.
    // pj_app's Media2DDockWidget deserializes the entry, opens FileVideoSource
    // on file_path, applies time_origin_ns as the wall-clock anchor (and, in
    // v3.0, clamps playback to [start_ns, end_ns] inside the shared file),
    // and drives the decoder from the global tracker (PJ4 ARCH §4.5). The MP4
    // bytes never reach ObjectStore — the file itself is the random-access
    // store.
    const PJ::sdk::SourceObjectWriteHostView* obj = objectWriteHost();
    if (obj == nullptr || model.camera_names.empty()) {
      return PJ::okStatus();
    }

    // Per-episode datasets: each episode is its own time domain starting at 0
    // (see rowTimestampNs above). The MP4's first frame corresponds to t=0
    // for v2.x (whole-file = one episode) and to t = -start_ns for v3.0
    // (the file starts before the clip; subtract start_ns so the episode's
    // first frame still lands on t=0 in the tracker's timeline).
    constexpr int64_t kEpisodeOriginNs = 0;

    const lerobot::EpisodeShard* shard = nullptr;
    if (auto it = model.episode_shards.find(ep); it != model.episode_shards.end()) {
      shard = &it->second;
    }

    const std::string meta = PJ::sdk::MediaMetadataBuilder()
                                 .extraString("builtin_object_type", "kAssetVideo")
                                 .schema(PJ::kSchemaAssetVideo)
                                 .build();

    for (const std::string& cam : model.camera_names) {
      auto otopic = obj->registerTopic("lerobot/" + cam, meta);
      if (!otopic) {
        return PJ::unexpected(otopic.error());
      }

      // Resolve per-camera clip metadata from the shard map when present
      // (v3.0). For v2.x the lookup falls through and clip_start/end stay
      // absent → whole-file playback, unchanged from the original behavior.
      const lerobot::VideoShard* video_shard = nullptr;
      if (shard != nullptr) {
        if (auto vit = shard->videos.find(cam); vit != shard->videos.end()) {
          video_shard = &vit->second;
        }
      }

      PJ::sdk::AssetVideo asset;
      asset.file_path = model.episodeVideo(ep, cam).string();
      asset.frame_rate = model.fps;
      if (video_shard != nullptr && video_shard->start_ns.has_value()) {
        // v3.0: align the episode's first frame to tracker t=0 by shifting
        // time_origin back by start_ns, then mark the clip window so the
        // consumer clamps playback. start_ns sits inside the file, ≥0, so
        // -start_ns yields ≤0 — a "pre-anchor" that aligns episode-local 0
        // with the actual first frame of this episode in the shared mp4.
        asset.time_origin_ns = PJ::Timestamp{-(*video_shard->start_ns)};
        asset.start_ns = video_shard->start_ns;
        asset.end_ns = video_shard->end_ns;
      } else {
        asset.time_origin_ns = PJ::Timestamp{kEpisodeOriginNs};
        // start_ns / end_ns left absent → whole-file playback for v2.x.
      }
      // media_type, width, height: defaults → PJ4 probes via FFmpeg.

      const std::vector<uint8_t> bytes = PJ::serializeAssetVideo(asset);
      auto pushed =
          obj->pushOwned(*otopic, PJ::Timestamp{kEpisodeOriginNs}, PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
      if (!pushed) {
        return PJ::unexpected(pushed.error());
      }
    }
    return PJ::okStatus();
  }

  LeRobotDialog dialog_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(LeRobotSource, kLerobotManifest)

PJ_DIALOG_PLUGIN(LeRobotDialog)
