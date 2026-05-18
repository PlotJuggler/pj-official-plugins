#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/media_metadata.hpp>

#include "lerobot_dialog.hpp"
#include "lerobot_manifest.hpp"

#include "dataset_model.hpp"
#include "flatten_plan.hpp"
#include "lerobot_arrow_helpers.hpp"
#include "timeline.hpp"
#include "video_ingest.hpp"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace ah = lerobot::arrow_helpers;

// One numeric output column. For a scalar parquet column `arrow_name` ==
// source name and `vec_k` == -1. For a flattened vector element, `arrow_name`
// is the list column and `vec_k` the child index.
struct OutColumn {
  std::string out_name;            // series name (stable, owns the string)
  std::string arrow_name;          // source parquet column
  int vec_k = -1;                  // -1 scalar; >=0 vector element index
  arrow::Type::type scalar_type = arrow::Type::NA;  // scalar only
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
    const std::vector<int64_t>& selected = dialog_.selectedEpisodes();
    if (selected.empty()) {
      return PJ::unexpected(std::string("no episodes selected"));
    }

    // Episode lengths in selection order → uniform per-episode offsets.
    std::vector<int64_t> lengths;
    lengths.reserve(selected.size());
    int64_t total_rows = 0;
    for (int64_t ep : selected) {
      int64_t len = 0;
      for (const auto& e : model->episodes) {
        if (e.episode_index == ep) {
          len = e.length;
          break;
        }
      }
      lengths.push_back(len);
      total_rows += len;
    }
    const auto offsets = lerobot::computeEpisodeOffsetsNs(lengths, model->fps, dialog_.gapSeconds());

    auto topic = writeHost().ensureTopic("lerobot");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }

    // Build the column plan once from the first selected episode's schema.
    auto plan_or = buildPlan(*model, selected.front());
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

    (void)runtimeHost().progressStart("Importing LeRobot", static_cast<uint64_t>(total_rows), true);

    int64_t processed = 0;
    // Per-camera frame timestamps, captured during the numeric pass so video
    // frames land on exactly the same ts as their parquet rows.
    std::vector<std::vector<int64_t>> ep_frame_ts(selected.size());

    for (std::size_t ei = 0; ei < selected.size(); ++ei) {
      auto st = importEpisode(*model, selected[ei], offsets[ei], plan, *topic, processed,
                              ep_frame_ts[ei]);
      if (!st) {
        return st;
      }
      if (runtimeHost().isStopRequested()) {
        return PJ::unexpected(std::string("import cancelled"));
      }
    }

    auto vst = importVideos(*model, selected, ep_frame_ts);
    if (!vst) {
      return vst;
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "LeRobot " + model->codebase_version + ": imported " + std::to_string(processed) +
            " rows from " + std::to_string(selected.size()) + " episode(s)");
    return PJ::okStatus();
  }

 private:
  PJ::Expected<std::vector<OutColumn>> buildPlan(const lerobot::DatasetModel& model, int64_t first_ep) {
    auto schema_or = openSchema(model.episodeParquet(first_ep).string());
    if (!schema_or) {
      return PJ::unexpected(schema_or.error());
    }
    const std::shared_ptr<arrow::Schema>& schema = *schema_or;

    std::vector<OutColumn> plan;
    std::vector<std::string> raw_names;  // for global dedupe
    for (int i = 0; i < schema->num_fields(); ++i) {
      const auto& field = schema->field(i);
      const std::string& name = field->name();
      const auto& type = field->type();
      if (name == "timestamp") {
        continue;  // synthesized into the time axis, not a series
      }
      if (ah::isScalarArrowType(type->id())) {
        OutColumn c;
        c.out_name = name;
        c.arrow_name = name;
        c.scalar_type = type->id();
        c.prim = ah::arrowTypeToPrimitive(type->id());
        plan.push_back(std::move(c));
        raw_names.push_back(name);
      } else if (ah::isFloatVectorColumn(type)) {
        int k = 0;
        if (type->id() == arrow::Type::FIXED_SIZE_LIST) {
          k = std::static_pointer_cast<arrow::FixedSizeListType>(type)->list_size();
        }
        const lerobot::FeatureSpec* fs = model.feature(name);
        if (k <= 0 && fs != nullptr && !fs->shape.empty()) {
          k = static_cast<int>(fs->shape.back());
        }
        const std::vector<std::string> labels = lerobot::flattenedFieldNames(
            name, k, fs != nullptr ? fs->names : std::vector<std::string>{});
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
      return PJ::unexpected(std::string("no supported columns in ") +
                            model.episodeParquet(first_ep).string());
    }
    const auto deduped = lerobot::dedupeFieldNames(raw_names);
    for (std::size_t i = 0; i < plan.size(); ++i) {
      plan[i].out_name = deduped[i];
    }
    return plan;
  }

  static PJ::Expected<std::shared_ptr<arrow::Schema>> openSchema(const std::string& path) {
    auto infile = arrow::io::ReadableFile::Open(path);
    if (!infile.ok()) {
      return PJ::unexpected("cannot open parquet: " + path);
    }
    auto reader = parquet::arrow::OpenFile(*infile, arrow::default_memory_pool());
    if (!reader.ok()) {
      return PJ::unexpected("failed to open parquet: " + path);
    }
    std::shared_ptr<arrow::Schema> schema;
    if (!(*reader)->GetSchema(&schema).ok()) {
      return PJ::unexpected("failed to read schema: " + path);
    }
    return schema;
  }

  PJ::Status importEpisode(
      const lerobot::DatasetModel& model, int64_t ep, int64_t offset,
      const std::vector<OutColumn>& plan, PJ::sdk::TopicHandle topic, int64_t& processed,
      std::vector<int64_t>& out_frame_ts) {
    const std::string path = model.episodeParquet(ep).string();
    auto infile = arrow::io::ReadableFile::Open(path);
    if (!infile.ok()) {
      return PJ::unexpected("cannot open parquet: " + path);
    }
    auto reader_or = parquet::arrow::OpenFile(*infile, arrow::default_memory_pool());
    if (!reader_or.ok()) {
      return PJ::unexpected("failed to open parquet: " + path);
    }
    auto reader = std::move(*reader_or);
    std::shared_ptr<arrow::Schema> schema;
    if (!reader->GetSchema(&schema).ok()) {
      return PJ::unexpected("failed to read schema: " + path);
    }

    auto col_index = [&](const std::string& n) -> int {
      for (int i = 0; i < schema->num_fields(); ++i) {
        if (schema->field(i)->name() == n) {
          return i;
        }
      }
      return -1;
    };
    const int ts_idx = col_index("timestamp");
    const int frame_idx = col_index("frame_index");

    std::shared_ptr<arrow::RecordBatchReader> batches;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (!reader->GetRecordBatchReader(&batches).ok()) {
      return PJ::unexpected("failed to create batch reader: " + path);
    }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    std::vector<PJ::sdk::NamedFieldValue> row_fields;
    row_fields.reserve(plan.size());
    std::shared_ptr<arrow::RecordBatch> batch;
    int64_t row_counter = 0;

    while (batches->ReadNext(&batch).ok() && batch) {
      const int64_t n = batch->num_rows();
      std::shared_ptr<arrow::Array> ts_arr =
          ts_idx >= 0 ? batch->column(ts_idx) : nullptr;
      std::shared_ptr<arrow::Array> fidx_arr =
          frame_idx >= 0 ? batch->column(frame_idx) : nullptr;

      for (int64_t r = 0; r < n; ++r) {
        const bool has_ts = ts_arr != nullptr && !ts_arr->IsNull(r);
        const double sec = has_ts ? readSeconds(ts_arr, r) : 0.0;
        const int64_t fi = fidx_arr ? readInt(fidx_arr, r, row_counter) : row_counter;
        const int64_t ts_ns = lerobot::rowTimestampNs(offset, has_ts, sec, fi, model.fps);

        row_fields.clear();
        for (const auto& c : plan) {
          const int ci = col_index(c.arrow_name);
          if (ci < 0) {
            continue;
          }
          const auto& arr = batch->column(ci);
          if (c.vec_k < 0) {
            auto v = ah::getArrowValueRef(arr, r, c.scalar_type);
            if (!PJ::sdk::isNull(v)) {
              row_fields.push_back({.name = c.out_name, .value = v});
            }
          } else {
            const auto cell = ah::floatVectorCell(arr, r);
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
        out_frame_ts.push_back(ts_ns);
        ++row_counter;
        ++processed;
      }
      (void)runtimeHost().progressUpdate(static_cast<uint64_t>(processed));
      if (runtimeHost().isStopRequested()) {
        return PJ::unexpected(std::string("import cancelled"));
      }
    }
    return PJ::okStatus();
  }

  PJ::Status importVideos(
      const lerobot::DatasetModel& model, const std::vector<int64_t>& selected,
      const std::vector<std::vector<int64_t>>& ep_frame_ts) {
    const PJ::sdk::SourceObjectWriteHostView* obj = objectWriteHost();
    if (obj == nullptr || model.camera_names.empty()) {
      return PJ::okStatus();  // no media host or no cameras → numeric only
    }
    for (const std::string& cam : model.camera_names) {
      const lerobot::FeatureSpec* fs = model.feature(cam);
      const std::string codec = fs != nullptr ? fs->video_codec : std::string{};
      const std::string meta = PJ::sdk::MediaMetadataBuilder()
                                   .mediaClass("video")
                                   .encoding(codec.empty() ? "h264" : codec)
                                   .schema("foxglove/CompressedVideo")
                                   .build();
      auto otopic = obj->registerTopic("lerobot/" + cam, meta);
      if (!otopic) {
        return PJ::unexpected(otopic.error());
      }
      for (std::size_t ei = 0; ei < selected.size(); ++ei) {
        const std::string mp4 = model.episodeVideo(selected[ei], cam).string();
        auto st = lerobot::ingestEpisodeVideo(*obj, *otopic, mp4, codec, ep_frame_ts[ei]);
        if (!st) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              "camera '" + cam + "' episode " + std::to_string(selected[ei]) + ": " + st.error());
        }
      }
    }
    return PJ::okStatus();
  }

  LeRobotDialog dialog_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(LeRobotSource, kLerobotManifest)

PJ_DIALOG_PLUGIN(LeRobotDialog)
