#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "blf_dialog.hpp"
#include "blf_frames.hpp"
#include "blf_manifest.hpp"

namespace {

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

// Imports Vector BLF CAN logs. Each CAN channel is decoded with its own DBC
// database(s) (per-channel mapping); decoded signals become timeseries grouped
// as one topic per (channel, message).
class BlfSource : public PJ::FileSourceBase {
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
    nlohmann::json mapping = nlohmann::json::object();
    for (const auto& [channel, paths] : channel_dbcs_) {
      mapping[std::to_string(channel)] = paths;
    }
    cfg["channel_dbcs"] = mapping;
    return cfg.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    const auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    filepath_ = cfg.value("filepath", std::string{});
    channel_dbcs_.clear();
    if (cfg.contains("channel_dbcs") && cfg["channel_dbcs"].is_object()) {
      for (const auto& [key, value] : cfg["channel_dbcs"].items()) {
        if (!value.is_array()) {
          continue;
        }
        const auto channel = static_cast<std::uint16_t>(std::stoul(key));
        auto& paths = channel_dbcs_[channel];
        for (const auto& entry : value) {
          if (entry.is_string()) {
            paths.push_back(entry.get<std::string>());
          }
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

    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(filepath_, ec);
    const std::uint64_t total = ec ? 0 : static_cast<std::uint64_t>(file_size);
    (void)runtimeHost().progressStart("Importing BLF", total, true);

    // One decoder per CAN channel, each loaded with that channel's DBC(s).
    std::unordered_map<std::uint16_t, mf4_detail::CanDecoder> decoders;
    for (const auto& [channel, paths] : channel_dbcs_) {
      auto& decoder = decoders[channel];
      for (const auto& dbc : paths) {
        if (auto status = decoder.loadDbcFile(dbc); !status) {
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, status.error());
        }
      }
    }
    if (decoders.empty()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "no DBC database assigned to any channel; nothing will be decoded");
    }

    std::unordered_map<std::string, PJ::sdk::TopicHandle> topics;
    std::vector<PJ::sdk::NamedFieldValue> row_fields;
    std::uint64_t decoded_frames = 0;
    std::uint64_t unmatched = 0;
    std::uint64_t no_dbc_channel = 0;
    std::uint64_t seen = 0;

    blf_detail::BlfStats stats;
    const auto status = blf_detail::readCanFrames(
        filepath_,
        [&](const blf_detail::CanFrame& frame) -> bool {
          if ((++seen % 4096) == 0 && runtimeHost().isStopRequested()) {
            return false;  // stop reading; cancellation handled below
          }
          const auto dit = decoders.find(frame.channel);
          if (dit == decoders.end()) {
            ++no_dbc_channel;
            return true;
          }
          bool matched = false;
          const auto signals = dit->second.decode(frame.can_id, frame.extended, frame.data, matched);
          if (!matched) {
            ++unmatched;
            return true;
          }
          if (signals.empty()) {
            return true;
          }
          ++decoded_frames;

          const std::string message = dit->second.messageName(frame.can_id, frame.extended);
          const std::string topic_name =
              "CAN/ch" + std::to_string(frame.channel) + "/" + (message.empty() ? hexId(frame.can_id) : message);
          auto it = topics.find(topic_name);
          if (it == topics.end()) {
            auto topic = writeHost().ensureTopic(topic_name);
            if (!topic) {
              return true;  // best-effort within the frame callback
            }
            it = topics.emplace(topic_name, *topic).first;
          }
          row_fields.clear();
          row_fields.reserve(signals.size());
          for (const auto& sig : signals) {
            row_fields.push_back({.name = sig.name, .value = sig.value});
          }
          (void)writeHost().appendRecord(
              it->second, PJ::Timestamp{frame.ts_ns},
              PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields.data(), row_fields.size()));
          return true;
        },
        stats);
    if (!status) {
      return status;
    }
    if (runtimeHost().isStopRequested()) {
      return PJ::unexpected(std::string("import cancelled"));
    }
    (void)runtimeHost().progressUpdate(total);

    std::string summary = "Decoded " + std::to_string(decoded_frames) + " CAN frame(s) into " +
                          std::to_string(topics.size()) + " message topic(s)";
    if (unmatched > 0) {
      summary += "; " + std::to_string(unmatched) + " frame(s) had no DBC match";
    }
    if (no_dbc_channel > 0) {
      summary += "; " + std::to_string(no_dbc_channel) + " frame(s) on channels with no DBC";
    }
    if (stats.skipped_objects > 0) {
      summary += "; " + std::to_string(stats.skipped_objects) + " non-CAN object(s) skipped";
    }
    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, summary);
    return PJ::okStatus();
  }

 private:
  std::string filepath_;
  std::unordered_map<std::uint16_t, std::vector<std::string>> channel_dbcs_;
  blf_detail::BlfDialog dialog_;
};

}  // namespace

PJ_DIALOG_PLUGIN(blf_detail::BlfDialog)
PJ_DATA_SOURCE_PLUGIN(BlfSource, kBlfManifest)
