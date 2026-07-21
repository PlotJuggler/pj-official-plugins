#include <cstdint>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/can_topic.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mf4_dialog.hpp"
#include "mf4_manifest.hpp"
#include "mf4_reader.hpp"

namespace {

/// mdf::BusType::CAN as an int (matches Mf4Reader::GroupInfo::bus_type).
constexpr int kCanBusType = 2;

/// Deterministic, collision-free topic name for a measurement group.
std::string uniqueTopicName(
    const mf4_detail::GroupInfo& group, std::size_t index, std::unordered_set<std::string>& used) {
  const std::string base = group.name.empty() ? (std::string("Group_") + std::to_string(index)) : group.name;
  return mf4_detail::dedupeName(base, used);
}

// The MF4 / MDF data source. Imports measurement channel groups as timeseries
// (one topic per group, one field per channel) and, when DBC databases are
// supplied, decodes CAN bus-logging groups into named physical signals.
class Mf4Source : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  // The dialog owns the config JSON round-trip; the host replaces the source
  // config with the dialog's saveConfig() after accept, so delegating keeps a
  // single source of truth (matches data_load_mcap/csv/parquet).
  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    const std::string& filepath = dialog_.filePath();
    if (filepath.empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    mf4_detail::Mf4Reader reader;
    if (auto status = reader.open(filepath); !status) {
      return status;
    }
    if (!reader.finalized()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "MF4 file is not finalized; some data may be missing");
    }

    // Load DBC database(s) for CAN decoding.
    pj_can_dbc::CanDecoder decoder;
    for (const auto& dbc : dialog_.dbcPaths()) {
      if (auto status = decoder.loadDbcFile(dbc); !status) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, status.error());
      }
    }
    const bool have_dbc = decoder.messageCount() > 0;

    const auto& groups = reader.groups();
    std::unordered_set<std::string> used_topics;
    // Keyed by (bus_channel, id, extended) packed numerically — same-id frames
    // from different buses must stay in separate topics, and the hot per-frame
    // lookup avoids building a string key.
    std::unordered_map<std::uint64_t, PJ::sdk::TopicHandle> can_topics;
    std::vector<PJ::sdk::NamedFieldValue> row_fields;
    std::size_t series_count = 0;
    std::size_t skipped_no_master = 0;
    std::size_t skipped_can_no_dbc = 0;
    std::uint64_t unmatched_frames = 0;
    std::uint64_t undecodable_frames = 0;
    std::uint64_t skipped_invalid_time = 0;

    // Progress in units of non-empty channel groups (per-group byte sizes are
    // not known up front). Cancel is polled inside each group as well.
    std::uint64_t total_groups = 0;
    for (const auto& group : groups) {
      total_groups += group.sample_count > 0 ? 1 : 0;
    }
    (void)runtimeHost().progressStart("Importing MF4", total_groups, true);
    std::uint64_t groups_done = 0;
    std::uint64_t rows_seen = 0;
    bool cancelled = false;
    // Poll cadence for isStopRequested inside a group's row/frame stream.
    constexpr std::uint64_t kCancelPollMask = 4095;

    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
      if (cancelled || runtimeHost().isStopRequested()) {
        return PJ::unexpected(std::string("import cancelled"));
      }
      const auto& group = groups[gi];
      if (group.sample_count == 0) {
        continue;  // empty channel group (CANedge files carry many)
      }

      if (group.bus_type == kCanBusType) {
        if (!have_dbc) {
          ++skipped_can_no_dbc;
          continue;
        }
        mf4_detail::ReadGroupStats can_stats;
        auto status = reader.readCanGroup(
            gi,
            [&](std::int64_t ts_ns, std::uint16_t bus_channel, std::uint32_t id, bool extended,
                const std::vector<std::uint8_t>& data) {
              if ((++rows_seen & kCancelPollMask) == 0 && runtimeHost().isStopRequested()) {
                cancelled = true;
                return false;  // abort mid-group
              }
              pj_can_dbc::DecodeResult result = pj_can_dbc::DecodeResult::kNoMatch;
              const auto signals = decoder.decode(id, extended, data, result);
              if (result == pj_can_dbc::DecodeResult::kNoMatch) {
                ++unmatched_frames;
                return true;
              }
              if (result == pj_can_dbc::DecodeResult::kUndecodable) {
                ++undecodable_frames;
                return true;
              }
              if (signals.empty()) {
                return true;  // decoded, but the message defines no plain signals
              }
              const std::uint64_t key = (static_cast<std::uint64_t>(bus_channel) << 33) |
                                        (static_cast<std::uint64_t>(id) << 1) | (extended ? 1u : 0u);
              auto it = can_topics.find(key);
              if (it == can_topics.end()) {
                const std::string topic_name =
                    pj_can_dbc::canTopicName(bus_channel, decoder.messageName(id, extended), id);
                auto topic = writeHost().ensureTopic(topic_name);
                if (!topic) {
                  return true;  // best-effort within the frame callback
                }
                it = can_topics.emplace(key, *topic).first;
              }
              row_fields.clear();
              row_fields.reserve(signals.size());
              for (const auto& sig : signals) {
                row_fields.push_back({.name = sig.name, .value = sig.value});
              }
              (void)writeHost().appendRecord(
                  it->second, PJ::Timestamp{ts_ns},
                  PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields.data(), row_fields.size()));
              return true;
            },
            &can_stats);
        if (!status) {
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, status.error());
        }
        skipped_invalid_time += can_stats.skipped_invalid_time;
        ++groups_done;
        if (!runtimeHost().progressUpdate(groups_done)) {
          cancelled = true;
        }
        continue;
      }

      // Measurement group.
      if (!group.has_master) {
        ++skipped_no_master;
        continue;
      }
      const std::string topic_name = uniqueTopicName(group, gi, used_topics);
      auto topic = writeHost().ensureTopic(topic_name);
      if (!topic) {
        return PJ::unexpected(topic.error());
      }
      const PJ::sdk::TopicHandle topic_handle = *topic;

      const auto field_names = reader.valueChannelNames(gi);
      // Pre-register fields with their column types (parallel to field_names).
      std::size_t fi = 0;
      for (const auto& channel : group.channels) {
        if (channel.is_master || channel.type == PJ::PrimitiveType::kUnspecified) {
          continue;
        }
        if (fi < field_names.size()) {
          (void)writeHost().ensureField(topic_handle, field_names[fi], channel.type);
        }
        ++fi;
      }

      mf4_detail::ReadGroupStats group_stats;
      auto status = reader.readGroup(
          gi,
          [&](std::int64_t ts_ns, const std::vector<mf4_detail::SampleValue>& values) {
            if ((++rows_seen & kCancelPollMask) == 0 && runtimeHost().isStopRequested()) {
              cancelled = true;
              return false;  // abort mid-group
            }
            row_fields.clear();
            row_fields.reserve(values.size());
            for (std::size_t k = 0; k < values.size() && k < field_names.size(); ++k) {
              const auto& value = values[k];
              if (!value.valid) {
                row_fields.push_back({.name = field_names[k], .value = PJ::NullValue{}});
              } else if (value.type == PJ::PrimitiveType::kString) {
                // string_view into value.text, which is alive for this callback.
                row_fields.push_back({.name = field_names[k], .value = std::string_view(value.text)});
              } else {
                row_fields.push_back({.name = field_names[k], .value = value.number});
              }
            }
            (void)writeHost().appendRecord(
                topic_handle, PJ::Timestamp{ts_ns},
                PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields.data(), row_fields.size()));
            return true;
          },
          &group_stats);
      if (!status) {
        return status;
      }
      skipped_invalid_time += group_stats.skipped_invalid_time;
      series_count += field_names.size();
      ++groups_done;
      if (!runtimeHost().progressUpdate(groups_done)) {
        cancelled = true;
      }
    }
    if (cancelled || runtimeHost().isStopRequested()) {
      return PJ::unexpected(std::string("import cancelled"));
    }

    std::string summary = "Imported " + std::to_string(series_count) + " timeseries";
    if (!can_topics.empty()) {
      summary += ", " + std::to_string(can_topics.size()) + " CAN message(s)";
    }
    if (unmatched_frames > 0) {
      summary += "; " + std::to_string(unmatched_frames) + " CAN frames had no DBC match";
    }
    if (undecodable_frames > 0) {
      summary += "; " + std::to_string(undecodable_frames) +
                 " matched CAN frame(s) could not be decoded (truncated or CAN FD payload)";
    }
    if (skipped_no_master > 0) {
      summary += "; " + std::to_string(skipped_no_master) + " group(s) skipped (no master channel)";
    }
    if (skipped_can_no_dbc > 0) {
      summary += "; " + std::to_string(skipped_can_no_dbc) + " CAN group(s) skipped (no DBC loaded)";
    }
    if (skipped_invalid_time > 0) {
      summary += "; " + std::to_string(skipped_invalid_time) + " sample(s) skipped (invalid timestamp)";
    }
    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, summary);
    return PJ::okStatus();
  }

 private:
  mf4_detail::Mf4Dialog dialog_;  // owns the config (filepath + dbc_paths)
};

}  // namespace

PJ_DIALOG_PLUGIN(mf4_detail::Mf4Dialog)
PJ_DATA_SOURCE_PLUGIN(Mf4Source, kMf4Manifest)
