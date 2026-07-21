#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/can_topic.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "blf_dialog.hpp"
#include "blf_frames.hpp"
#include "blf_manifest.hpp"

namespace {

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
        const auto channel = blf_detail::parseChannelKey(key);
        if (!channel || !value.is_array()) {
          continue;  // hand-edited config: skip the bad entry, keep the rest
        }
        auto& paths = channel_dbcs_[*channel];
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
    // The dialog's saveConfig() replaces this config after accept, so it must
    // carry every channel's full DBC list, not just what the pickers edited.
    dialog_.setChannelDbcs({channel_dbcs_.begin(), channel_dbcs_.end()});
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
    std::uint64_t undecodable = 0;
    std::uint64_t no_dbc_channel = 0;
    std::uint64_t seen = 0;
    bool cancelled = false;

    blf_detail::BlfStats stats;
    const auto status = blf_detail::readCanFrames(
        filepath_,
        [&](const blf_detail::CanFrame& frame) -> bool {
          if ((++seen % 4096) == 0) {
            if (runtimeHost().isStopRequested()) {
              cancelled = true;
              return false;  // stop reading; cancellation handled below
            }
            // Advance the byte-denominated bar via the header's object count
            // (stats counts advance live during the read).
            if (total > 0 && stats.total_objects > 0) {
              const std::uint64_t done = seen + stats.skipped_objects;
              const std::uint64_t scaled = total * std::min(done, stats.total_objects) / stats.total_objects;
              if (!runtimeHost().progressUpdate(scaled)) {
                cancelled = true;
                return false;  // user cancelled from the progress dialog
              }
            }
          }
          const auto dit = decoders.find(frame.channel);
          if (dit == decoders.end()) {
            ++no_dbc_channel;
            return true;
          }
          mf4_detail::DecodeResult result = mf4_detail::DecodeResult::kNoMatch;
          const auto signals = dit->second.decode(frame.can_id, frame.extended, frame.data, result);
          if (result == mf4_detail::DecodeResult::kNoMatch) {
            ++unmatched;
            return true;
          }
          if (result == mf4_detail::DecodeResult::kUndecodable) {
            ++undecodable;
            return true;
          }
          if (signals.empty()) {
            return true;  // decoded, but the message defines no plain signals
          }
          ++decoded_frames;

          const std::string topic_name = mf4_detail::canTopicName(
              frame.channel, dit->second.messageName(frame.can_id, frame.extended), frame.can_id);
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
    if (cancelled || runtimeHost().isStopRequested()) {
      return PJ::unexpected(std::string("import cancelled"));
    }
    (void)runtimeHost().progressUpdate(total);

    std::string summary = "Decoded " + std::to_string(decoded_frames) + " CAN frame(s) into " +
                          std::to_string(topics.size()) + " message topic(s)";
    if (unmatched > 0) {
      summary += "; " + std::to_string(unmatched) + " frame(s) had no DBC match";
    }
    if (undecodable > 0) {
      summary += "; " + std::to_string(undecodable) + " matched frame(s) could not be decoded (truncated frame)";
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
