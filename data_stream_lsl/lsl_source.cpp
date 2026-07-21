#include <lsl_cpp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lsl_conversions.hpp"
#include "lsl_dialog.hpp"
#include "lsl_manifest.hpp"

namespace {

constexpr double kResolveTimeout = 2.0;   // seconds, blocking is OK in onStart
constexpr double kTimeCorrTimeout = 2.0;  // seconds
constexpr int kInletBufferSeconds = 360;  // liblsl default max_buflen

class LslSource : public PJ::StreamSourceBase {
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
    if (!config_json.empty()) {
      (void)dialog_.loadConfig(config_json);
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    const pj_lsl::DialogConfig cfg = pj_lsl::parseConfig(dialog_.saveConfig());
    mode_ = cfg.mode;
    if (cfg.streams.empty()) {
      return PJ::unexpected("no LSL streams selected");
    }

    inlets_.clear();
    // local_clock() -> epoch offset, captured once for all inlets.
    epoch_offset_ns_ = nowEpochNs() - static_cast<int64_t>(lsl::local_clock() * 1e9);

    // Disambiguated topic names, one per selected stream (order preserved).
    std::vector<pj_lsl::StreamKey> keys;
    keys.reserve(cfg.streams.size());
    for (const auto& s : cfg.streams) {
      keys.push_back({s.name, s.source_id});
    }
    const std::vector<std::string> topic_names = pj_lsl::uniqueTopicNames(keys);

    // Build into a local vector; only publish to inlets_ once the whole loop
    // succeeds, so a mid-loop host error (ensureTopic/ensureField) leaves no
    // half-opened inlets behind (startup is resource-transactional).
    std::vector<Inlet> pending;
    for (size_t i = 0; i < cfg.streams.size(); ++i) {
      const auto& sel = cfg.streams[i];
      std::optional<lsl::stream_info> info_opt = resolveOne(sel);
      if (!info_opt) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, "LSL stream not found, skipping: " + streamLabel(sel));
        continue;
      }
      lsl::stream_info& info = *info_opt;

      Inlet inlet;
      inlet.format = info.channel_format();
      inlet.field_type = pj_lsl::mapChannelFormat(inlet.format);
      if (inlet.field_type == PJ::PrimitiveType::kUnspecified) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, "LSL stream has undefined format, skipping: " + info.name());
        continue;
      }
      inlet.is_string = pj_lsl::isStringFormat(inlet.format);

      try {
        // recover=false: honor the no-reconnect design. A stream that drops is
        // reported lost via pull_chunk (see drain*), marked dead, and skipped;
        // it is not silently re-resolved.
        inlet.inlet = std::make_unique<lsl::stream_inlet>(info, kInletBufferSeconds, 0, false);
      } catch (const std::exception& e) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, std::string("failed to open LSL inlet: ") + e.what());
        continue;
      }

      try {
        inlet.time_corr_s = inlet.inlet->time_correction(kTimeCorrTimeout);
      } catch (...) {
        inlet.time_corr_s = 0.0;  // no time sync available -> treat as zero offset
      }

      auto topic = writeHost().ensureTopic(topic_names[i]);
      if (!topic) {
        return PJ::unexpected(topic.error());
      }
      inlet.topic = *topic;

      const std::vector<std::string> labels = pj_lsl::channelLabels(info);
      inlet.fields.reserve(labels.size());
      for (const auto& label : labels) {
        auto field = writeHost().ensureField(inlet.topic, label, inlet.field_type);
        if (!field) {
          return PJ::unexpected(field.error());
        }
        inlet.fields.push_back(*field);
      }

      pending.push_back(std::move(inlet));
    }

    if (pending.empty()) {
      return PJ::unexpected("no selected LSL stream could be resolved");
    }
    inlets_ = std::move(pending);
    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo, "LSL: streaming " + std::to_string(inlets_.size()) + " stream(s)");
    return PJ::okStatus();
  }

  // Drain every live inlet. A per-inlet stream loss marks that inlet dead and is
  // skipped thereafter; the other streams keep flowing (a single drop must not
  // stop the whole source). Only a host write failure is fatal.
  PJ::Status onPoll() override {
    const int64_t now_ns = nowEpochNs();
    for (auto& inlet : inlets_) {
      if (inlet.dead) {
        continue;
      }
      PJ::Status st = PJ::okStatus();
      if (inlet.is_string) {
        st = drainString(inlet, now_ns);
      } else if (inlet.format == lsl::cf_int64) {
        st = drainInt64(inlet, now_ns);  // native path: double can't hold int64 losslessly
      } else {
        st = drainNumeric(inlet, now_ns);
      }
      if (!st) {
        return st;  // host write failure -> fatal
      }
    }
    return PJ::okStatus();
  }

  void onStop() override {
    inlets_.clear();  // resetting each inlet releases its receiver thread + socket
  }

 private:
  struct Inlet {
    std::unique_ptr<lsl::stream_inlet> inlet;
    PJ::sdk::TopicHandle topic;
    std::vector<PJ::sdk::FieldHandle> fields;
    lsl::channel_format_t format = lsl::cf_undefined;
    PJ::PrimitiveType field_type = PJ::PrimitiveType::kFloat64;
    bool is_string = false;
    bool dead = false;  // stream lost -> stop draining this inlet, keep the rest
    double time_corr_s = 0.0;
  };

  // A stream that dropped: report once, mark dead, and let onPoll skip it.
  // Non-fatal to the source (the documented "dropped stream is skipped").
  void markLost(Inlet& inlet, const char* what) {
    inlet.dead = true;
    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kWarning, std::string("LSL stream lost, skipping: ") + what);
  }

  static int64_t nowEpochNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static std::string streamLabel(const pj_lsl::SelectedStream& s) {
    if (!s.name.empty()) {
      return s.name;
    }
    return s.source_id.empty() ? "<unnamed>" : s.source_id;
  }

  // Resolve a selected stream, preferring source_id, else name; when multiple
  // match, prefer one whose type also matches.
  static std::optional<lsl::stream_info> resolveOne(const pj_lsl::SelectedStream& sel) {
    std::vector<lsl::stream_info> found;
    try {
      if (!sel.source_id.empty()) {
        found = lsl::resolve_stream("source_id", sel.source_id, 1, kResolveTimeout);
      } else {
        found = lsl::resolve_stream("name", sel.name, 1, kResolveTimeout);
      }
    } catch (...) {
      return std::nullopt;
    }
    if (found.empty()) {
      return std::nullopt;
    }
    for (auto& info : found) {
      if (!sel.type.empty() && info.type() == sel.type) {
        return info;
      }
    }
    return found.front();
  }

  PJ::Status drainNumeric(Inlet& inlet, int64_t now_ns) {
    std::vector<std::vector<double>> chunk;
    std::vector<double> stamps;
    try {
      inlet.inlet->pull_chunk(chunk, stamps);  // two-arg overload: non-blocking drain
    } catch (const std::exception& e) {
      markLost(inlet, e.what());
      return PJ::okStatus();  // per-inlet loss is non-fatal
    }
    const size_t n = std::min(chunk.size(), stamps.size());
    for (size_t si = 0; si < n; ++si) {
      const int64_t ts = pj_lsl::computeTimestampNs(mode_, stamps[si], inlet.time_corr_s, epoch_offset_ns_, now_ns);
      std::vector<PJ::sdk::BoundFieldValue> fields;
      fields.reserve(inlet.fields.size());
      const size_t nch = std::min(inlet.fields.size(), chunk[si].size());
      for (size_t ci = 0; ci < nch; ++ci) {
        fields.push_back({inlet.fields[ci], pj_lsl::numericValueRef(inlet.field_type, chunk[si][ci])});
      }
      if (auto st = writeHost().appendBoundRecord(inlet.topic, PJ::Timestamp{ts}, fields); !st) {
        return st;
      }
    }
    return PJ::okStatus();
  }

  // cf_int64 has no lossless double representation; pull it natively so values
  // are exact and there is no out-of-range double->int64 cast (UB).
  PJ::Status drainInt64(Inlet& inlet, int64_t now_ns) {
    std::vector<std::vector<int64_t>> chunk;
    std::vector<double> stamps;
    try {
      inlet.inlet->pull_chunk(chunk, stamps);
    } catch (const std::exception& e) {
      markLost(inlet, e.what());
      return PJ::okStatus();
    }
    const size_t n = std::min(chunk.size(), stamps.size());
    for (size_t si = 0; si < n; ++si) {
      const int64_t ts = pj_lsl::computeTimestampNs(mode_, stamps[si], inlet.time_corr_s, epoch_offset_ns_, now_ns);
      std::vector<PJ::sdk::BoundFieldValue> fields;
      fields.reserve(inlet.fields.size());
      const size_t nch = std::min(inlet.fields.size(), chunk[si].size());
      for (size_t ci = 0; ci < nch; ++ci) {
        fields.push_back({inlet.fields[ci], PJ::sdk::ValueRef{chunk[si][ci]}});
      }
      if (auto st = writeHost().appendBoundRecord(inlet.topic, PJ::Timestamp{ts}, fields); !st) {
        return st;
      }
    }
    return PJ::okStatus();
  }

  PJ::Status drainString(Inlet& inlet, int64_t now_ns) {
    std::vector<std::vector<std::string>> chunk;
    std::vector<double> stamps;
    try {
      inlet.inlet->pull_chunk(chunk, stamps);
    } catch (const std::exception& e) {
      markLost(inlet, e.what());
      return PJ::okStatus();
    }
    const size_t n = std::min(chunk.size(), stamps.size());
    for (size_t si = 0; si < n; ++si) {
      const int64_t ts = pj_lsl::computeTimestampNs(mode_, stamps[si], inlet.time_corr_s, epoch_offset_ns_, now_ns);
      std::vector<PJ::sdk::BoundFieldValue> fields;
      fields.reserve(inlet.fields.size());
      const size_t nch = std::min(inlet.fields.size(), chunk[si].size());
      for (size_t ci = 0; ci < nch; ++ci) {
        fields.push_back({inlet.fields[ci], std::string_view(chunk[si][ci])});
      }
      if (auto st = writeHost().appendBoundRecord(inlet.topic, PJ::Timestamp{ts}, fields); !st) {
        return st;
      }
    }
    return PJ::okStatus();
  }

  LslDialog dialog_;
  std::vector<Inlet> inlets_;
  pj_lsl::TimestampMode mode_ = pj_lsl::TimestampMode::kSync;
  int64_t epoch_offset_ns_ = 0;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(LslSource, kLslManifest)

PJ_DIALOG_PLUGIN(LslDialog, kLslManifest)
