#pragma once

#include <algorithm>
#include <cctype>
#include <mcap/reader.hpp>
#include <nlohmann/json.hpp>
#include <pj_always_included/always_included.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mcap_helpers.hpp"

namespace {

// Generated at configure time
#include "dialog_mcap_ui.hpp"
#include "mcap_manifest.hpp"

struct ChannelInfo {
  std::string topic;
  std::string schema;
  std::string encoding;
  uint64_t msg_count = 0;
};

/// Channels of a type in PJ::AlwaysIncluded::kRules are always loaded when they hold messages
/// (see always_included.hpp for why, and how the streamers differ).
bool isAlwaysIncluded(const ChannelInfo& ch) {
  return PJ::AlwaysIncluded::matches(ch.schema, ch.encoding);
}

class McapDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  const std::string& filepath() const {
    return filepath_;
  }
  unsigned maxArraySize() const {
    return max_array_size_;
  }
  bool clampLargeArrays() const {
    return clamp_large_arrays_;
  }
  /// When true, use logTime from the MCAP envelope; otherwise use publishTime.
  bool useLogTime() const {
    return use_log_time_;
  }
  /// When true, the parser extracts the timestamp from inside the message payload (e.g. ROS Header.stamp) via
  /// use_embedded_timestamp in its config.
  bool useHeaderTimestamp() const {
    return use_header_timestamp_;
  }
  const std::unordered_set<std::string>& selectedTopics() const {
    return selected_topics_;
  }
  /// Non-empty when the file could not be analyzed (unreadable, damaged beyond the reader's fallback scan). Shown in
  /// the dialog so an empty channel table is never mistaken for a recording without topics.
  const std::string& analyzeError() const {
    return analyze_error_;
  }

  std::string manifest() const override {
    return kMcapManifest;
  }

  std::string ui_content() const override {
    return kDialogMcapUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    wd.setRange("spinBox", 0, 9999);
    wd.setValue("spinBox", static_cast<int>(max_array_size_));
    wd.setChecked("radioClamp", clamp_large_arrays_);
    wd.setChecked("radioSkip", !clamp_large_arrays_);

    wd.setChecked("radioPublishTime", !use_log_time_);
    wd.setChecked("radioLogTime", use_log_time_);
    wd.setChecked("checkBoxUseTimestamp", use_header_timestamp_);

    wd.setText("lineEditFilter", filter_text_);

    auto filtered = filteredChannels();
    std::vector<std::string> headers = {"Channel name", "Schema", "Encoding", "Msg Count"};
    wd.setTableHeaders("tableWidget", headers);

    std::vector<std::vector<PJ::TableItem>> rows;
    std::vector<std::string> selected_topic_names;
    std::vector<int> disabled_row_indices;
    rows.reserve(filtered.size());

    for (size_t i = 0; i < filtered.size(); ++i) {
      const auto& ch = *filtered[i];
      // Msg Count carries its native uint64 so the column sorts numerically (the host could only compare the digits).
      rows.push_back({ch.topic, ch.schema, ch.encoding, PJ::TableItem(ch.msg_count)});
      if (selected_topics_.count(ch.topic) > 0) {
        selected_topic_names.push_back(ch.topic);
      }
      bool locked_always_included = ch.msg_count > 0 && isAlwaysIncluded(ch);
      if (ch.msg_count == 0 || locked_always_included) {
        disabled_row_indices.push_back(static_cast<int>(i));
      }
      if (locked_always_included) {
        wd.setCellTooltip("tableWidget", static_cast<int>(i), 0, PJ::AlwaysIncluded::kTooltip);
      }
    }
    wd.setTableRows("tableWidget", rows);
    wd.setDisabledRows("tableWidget", disabled_row_indices);
    // Restore the selection by first-column text (the topic name), not row index: the host matches items by text, which
    // is sort-agnostic, so the selection survives the table's built-in column sorting (sortingEnabled=true).
    wd.setSelectedItems("tableWidget", selected_topic_names);

    wd.setShortcut("btnSelectAll", "Ctrl+A");
    wd.setShortcut("btnDeselectAll", "Ctrl+Shift+A");

    // Explains an empty table, which would otherwise look like a recording with no topics.
    wd.setVisible("labelAnalyzeError", !analyze_error_.empty());
    if (!analyze_error_.empty()) {
      wd.setLabel("labelAnalyzeError", analyze_error_);
    }

    wd.setOkEnabled(!selected_topics_.empty());

    return wd.toJson();
  }

  bool onValueChanged(std::string_view widget_name, int value) override {
    if (widget_name == "spinBox") {
      max_array_size_ = static_cast<unsigned>(std::max(0, value));
      return false;
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxUseTimestamp") {
      use_header_timestamp_ = checked;
      return false;
    }
    if (!checked) {
      return false;
    }
    if (widget_name == "radioClamp") {
      clamp_large_arrays_ = true;
      return true;
    }
    if (widget_name == "radioSkip") {
      clamp_large_arrays_ = false;
      return true;
    }
    if (widget_name == "radioPublishTime") {
      use_log_time_ = false;
      return false;
    }
    if (widget_name == "radioLogTime") {
      use_log_time_ = true;
      return false;
    }
    return false;
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditFilter") {
      filter_text_ = std::string(text);
      return true;  // rebuild table with filtered rows
    }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name == "tableWidget") {
      // The table only reports the rows visible under the filter, so a plain clear()+re-add would drop any selection
      // the filter is hiding. Reconcile only the visible topics and leave everything the filter hides untouched.
      std::unordered_set<std::string> visible;
      for (const auto* ch : filteredChannels()) {
        visible.insert(ch->topic);
      }
      for (auto it = selected_topics_.begin(); it != selected_topics_.end();) {
        if (visible.count(*it) > 0) {
          it = selected_topics_.erase(it);
        } else {
          ++it;
        }
      }
      for (const auto& topic : selected) {
        selected_topics_.insert(topic);
      }
      reassertAlwaysIncluded();
      return true;  // update OK button state
    }
    return false;
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "btnSelectAll") {
      auto filtered = filteredChannels();
      for (const auto* ch : filtered) {
        if (ch->msg_count > 0) {
          selected_topics_.insert(ch->topic);
        }
      }
      return true;
    }
    if (widget_name == "btnDeselectAll") {
      selected_topics_.clear();
      reassertAlwaysIncluded();
      return true;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["filepath"] = filepath_;
    cfg["max_array_size"] = max_array_size_;
    cfg["clamp_large_arrays"] = clamp_large_arrays_;
    cfg["use_log_time"] = use_log_time_;
    cfg["use_header_timestamp"] = use_header_timestamp_;
    cfg["selected_topics"] = std::vector<std::string>(selected_topics_.begin(), selected_topics_.end());
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }

    filepath_ = cfg.value("filepath", std::string{});
    max_array_size_ = cfg.value("max_array_size", 500u);
    clamp_large_arrays_ = cfg.value("clamp_large_arrays", true);
    use_log_time_ = cfg.value("use_log_time", cfg.value("use_mcap_log_time", false));
    // "use_timestamp" is the legacy key of this option; it keeps older saved configs loading.
    use_header_timestamp_ = cfg.value("use_header_timestamp", cfg.value("use_timestamp", false));

    selected_topics_.clear();
    if (auto it = cfg.find("selected_topics"); it != cfg.end() && it->is_array()) {
      for (const auto& t : *it) {
        if (t.is_string()) {
          selected_topics_.insert(t.get<std::string>());
        }
      }
    }

    if (!filepath_.empty()) {
      analyzeFile();
    }
    return true;
  }

 private:
  void analyzeFile() {
    all_channels_.clear();
    analyze_error_.clear();

    auto describe = [](const mcap::Status& s) {
      return "Code: " + std::to_string(static_cast<int>(s.code)) + "\nMessage: " + s.message;
    };

    // A restored config still holds the PREVIOUS recording's selection. The pruning step near the end normally
    // reconciles it, so any early return must drop it here: otherwise OK stays enabled over an empty table and starts
    // an import that can bind nothing.
    const auto fail = [this](std::string message) {
      analyze_error_ = std::move(message);
      selected_topics_.clear();
    };

    const auto recording = PJ::McapHelpers::resolveRecordingFiles(filepath_);
    if (!recording.error.empty()) {
      fail(recording.error);
      return;
    }
    const bool multi_file = recording.paths.size() > 1;

    // A split rosbag2 bag repeats its channels in every file: merge them by topic and sum the message counts.
    std::unordered_map<std::string, size_t> channel_index_by_topic;
    size_t channels_with_dangling_schema = 0;
    for (const std::string& path : recording.paths) {
      const std::string where = multi_file ? "\n" + path : std::string{};
      mcap::McapReader reader;
      auto status = reader.open(path);
      if (!status.ok()) {
        fail("Cannot open this MCAP file." + where + "\n" + describe(status));
        return;
      }

      // A failure here means neither the summary nor the data section could be read: readChannelSummary already falls
      // back to a scan for a bad summary.
      PJ::McapHelpers::McapSummaryInfo summary;
      status = PJ::McapHelpers::readChannelSummary(reader, summary);
      if (!status.ok()) {
        fail("Cannot read the contents of this MCAP file." + where + "\n" + describe(status));
        reader.close();
        return;
      }

      std::unordered_map<mcap::ChannelId, uint64_t> msg_counts;
      if (summary.statistics) {
        msg_counts = summary.statistics->channelMessageCounts;
      }

      const auto& schemas = summary.schemas;
      for (const auto& [id, channel_ptr] : summary.channels) {
        // schema_id 0 is the MCAP spec's "no schema" sentinel, not an error: the channel is self-describing (schemaless
        // JSON is the common case) and binds on its message encoding alone. Schema ids are 1-based, so a NON-zero id
        // that resolves to nothing is a dangling reference in a damaged file: no parser can bind to it, and offering it
        // in the picker would promise data that never arrives.
        const bool schemaless = PJ::McapHelpers::isSchemaless(channel_ptr->schemaId);
        auto schema_it = schemas.find(channel_ptr->schemaId);
        if (!schemaless && schema_it == schemas.end()) {
          ++channels_with_dangling_schema;
          continue;
        }

        auto count_it = msg_counts.find(id);
        const uint64_t msg_count = (count_it != msg_counts.end()) ? count_it->second : 0;

        if (auto known = channel_index_by_topic.find(channel_ptr->topic); known != channel_index_by_topic.end()) {
          all_channels_[known->second].msg_count += msg_count;
          continue;
        }

        ChannelInfo info;
        info.topic = channel_ptr->topic;
        if (schemaless) {
          info.encoding = channel_ptr->messageEncoding;
        } else {
          info.schema = schema_it->second->name;
          info.encoding =
              channel_ptr->messageEncoding.empty() ? schema_it->second->encoding : channel_ptr->messageEncoding;
        }
        info.msg_count = msg_count;

        channel_index_by_topic.emplace(info.topic, all_channels_.size());
        all_channels_.push_back(std::move(info));
      }
      reader.close();
    }

    if (all_channels_.empty()) {
      analyze_error_ = channels_with_dangling_schema > 0
                           ? "No readable channels: all " + std::to_string(channels_with_dangling_schema) +
                                 " channel(s) in this file reference a missing schema."
                           : "No channels were found in this MCAP file.";
    }

    std::sort(all_channels_.begin(), all_channels_.end(), [](const ChannelInfo& a, const ChannelInfo& b) {
      return a.topic < b.topic;
    });

    // Saved selections belong to the previously opened recording. Drop names that do not exist (or have no messages) in
    // this one; otherwise a stale, non-empty set leaves the dialog with no selected rows and OK disabled.
    std::unordered_set<std::string> selectable_topics;
    for (const auto& ch : all_channels_) {
      if (ch.msg_count > 0) {
        selectable_topics.insert(ch.topic);
      }
    }
    std::erase_if(selected_topics_, [&](const std::string& topic) { return !selectable_topics.contains(topic); });

    // If no previous selection survives, select all channels with messages.
    if (selected_topics_.empty()) {
      selected_topics_ = std::move(selectable_topics);
    }

    reassertAlwaysIncluded();
  }

  /// Re-adds the always-included channels (3D rendering depends on them even when the user narrows the selection).
  /// Idempotent: call it after any mutation of selected_topics_.
  void reassertAlwaysIncluded() {
    for (const auto& ch : all_channels_) {
      if (ch.msg_count > 0 && isAlwaysIncluded(ch)) {
        selected_topics_.insert(ch.topic);
      }
    }
  }

  std::vector<const ChannelInfo*> filteredChannels() const {
    std::vector<const ChannelInfo*> result;
    if (filter_text_.empty()) {
      for (const auto& ch : all_channels_) {
        result.push_back(&ch);
      }
      return result;
    }

    auto lower = [](std::string s) {
      for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return s;
    };

    // Space-separated words, ALL of which must match, case-insensitively like the streaming pickers' filters.
    std::vector<std::string> words;
    std::string word;
    for (char c : filter_text_) {
      if (c == ' ') {
        if (!word.empty()) {
          words.push_back(lower(word));
          word.clear();
        }
      } else {
        word += c;
      }
    }
    if (!word.empty()) {
      words.push_back(lower(word));
    }

    for (const auto& ch : all_channels_) {
      const std::string topic = lower(ch.topic);
      bool match = true;
      for (const auto& w : words) {
        if (topic.find(w) == std::string::npos) {
          match = false;
          break;
        }
      }
      if (match) {
        result.push_back(&ch);
      }
    }
    return result;
  }

  std::string analyze_error_;
  std::string filepath_;
  unsigned max_array_size_ = 500;
  bool clamp_large_arrays_ = true;
  bool use_log_time_ = false;
  bool use_header_timestamp_ = false;
  std::unordered_set<std::string> selected_topics_;
  std::string filter_text_;

  std::vector<ChannelInfo> all_channels_;
};

}  // namespace
