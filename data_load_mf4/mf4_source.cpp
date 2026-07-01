#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <string>
#include <string_view>
#include <system_error>
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
  std::string name = base;
  int suffix = 1;
  while (used.count(name) != 0) {
    name = base + "#" + std::to_string(suffix);
    ++suffix;
  }
  used.insert(name);
  return name;
}

/// Hex "0xNN" rendering of a CAN id (fallback topic name when a message has no
/// DBC name).
std::string hexId(std::uint32_t id) {
  static const char* const kHex = "0123456789ABCDEF";
  std::string out = "0x";
  bool started = false;
  for (int shift = 28; shift >= 0; shift -= 4) {
    const auto nibble = static_cast<std::size_t>((id >> shift) & 0xFu);
    if (nibble != 0 || started || shift == 0) {
      out.push_back(kHex[nibble]);
      started = true;
    }
  }
  return out;
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

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["filepath"] = filepath_;
    cfg["dbc_paths"] = dbc_paths_;
    return cfg.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    const auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    filepath_ = cfg.value("filepath", std::string{});
    dbc_paths_.clear();
    if (cfg.contains("dbc_paths") && cfg["dbc_paths"].is_array()) {
      for (const auto& entry : cfg["dbc_paths"]) {
        if (entry.is_string()) {
          dbc_paths_.push_back(entry.get<std::string>());
        }
      }
    }
    if (!filepath_.empty()) {
      dialog_.setFilePath(filepath_);
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    if (filepath_.empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    mf4_detail::Mf4Reader reader;
    if (auto status = reader.open(filepath_); !status) {
      return status;
    }
    if (!reader.finalized()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "MF4 file is not finalized; some data may be missing");
    }

    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(filepath_, ec);
    const std::uint64_t total = ec ? 0 : static_cast<std::uint64_t>(file_size);
    (void)runtimeHost().progressStart("Importing MF4", total, true);

    // Load DBC database(s) for CAN decoding.
    mf4_detail::CanDecoder decoder;
    for (const auto& dbc : dbc_paths_) {
      if (auto status = decoder.loadDbcFile(dbc); !status) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, status.error());
      }
    }
    const bool have_dbc = decoder.messageCount() > 0;

    const auto& groups = reader.groups();
    std::unordered_set<std::string> used_topics;
    std::unordered_map<std::string, PJ::sdk::TopicHandle> can_topics;
    std::vector<PJ::sdk::NamedFieldValue> row_fields;
    std::size_t series_count = 0;
    std::size_t skipped_no_master = 0;
    std::size_t skipped_can_no_dbc = 0;
    std::uint64_t unmatched_frames = 0;

    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
      if (runtimeHost().isStopRequested()) {
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
        auto status = reader.readCanGroup(
            gi, [&](std::int64_t ts_ns, std::uint32_t id, bool extended, const std::vector<std::uint8_t>& data) {
              bool matched = false;
              const auto signals = decoder.decode(id, extended, data, matched);
              if (!matched) {
                ++unmatched_frames;
                return;
              }
              if (signals.empty()) {
                return;
              }
              const std::string key = std::to_string(id) + (extended ? "e" : "s");
              auto it = can_topics.find(key);
              if (it == can_topics.end()) {
                std::string name = decoder.messageName(id, extended);
                const std::string topic_name = std::string("CAN/") + (name.empty() ? hexId(id) : name);
                auto topic = writeHost().ensureTopic(topic_name);
                if (!topic) {
                  return;  // best-effort within the frame callback
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
            });
        if (!status) {
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, status.error());
        }
        (void)runtimeHost().progressUpdate(total);
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

      auto status = reader.readGroup(gi, [&](std::int64_t ts_ns, const std::vector<mf4_detail::SampleValue>& values) {
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
      });
      if (!status) {
        return status;
      }
      series_count += field_names.size();
      (void)runtimeHost().progressUpdate(total);
    }

    std::string summary = "Imported " + std::to_string(series_count) + " timeseries";
    if (!can_topics.empty()) {
      summary += ", " + std::to_string(can_topics.size()) + " CAN message(s)";
    }
    if (unmatched_frames > 0) {
      summary += "; " + std::to_string(unmatched_frames) + " CAN frames had no DBC match";
    }
    if (skipped_no_master > 0) {
      summary += "; " + std::to_string(skipped_no_master) + " group(s) skipped (no master channel)";
    }
    if (skipped_can_no_dbc > 0) {
      summary += "; " + std::to_string(skipped_can_no_dbc) + " CAN group(s) skipped (no DBC loaded)";
    }
    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, summary);
    return PJ::okStatus();
  }

 private:
  std::string filepath_;
  std::vector<std::string> dbc_paths_;
  mf4_detail::Mf4Dialog dialog_;
};

}  // namespace

PJ_DIALOG_PLUGIN(mf4_detail::Mf4Dialog)
PJ_DATA_SOURCE_PLUGIN(Mf4Source, kMf4Manifest)
