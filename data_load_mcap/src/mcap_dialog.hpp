#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include <mcap/reader.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// Generated from ui/dialog_mcap.ui at configure time
#include "dialog_mcap_ui.hpp"

struct ChannelInfo {
  std::string topic;
  std::string schema;
  std::string encoding;
  uint64_t msg_count = 0;
};

class McapDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  // --- Accessors for McapSource ---
  const std::string& filepath() const { return filepath_; }
  unsigned maxArraySize() const { return max_array_size_; }
  bool clampLargeArrays() const { return clamp_large_arrays_; }
  bool useTimestamp() const { return use_timestamp_; }
  bool useMcapLogTime() const { return use_mcap_log_time_; }
  const std::unordered_set<std::string>& selectedTopics() const { return selected_topics_; }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return R"({"name":"MCAP File Source","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kDialogMcapUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Array size controls
    wd.setValue("spinBox", static_cast<int>(max_array_size_));
    wd.setChecked("radioClamp", clamp_large_arrays_);
    wd.setChecked("radioSkip", !clamp_large_arrays_);

    // Timestamp controls
    wd.setChecked("checkBoxUseTimestamp", use_timestamp_);
    wd.setChecked("radioPubTime", !use_mcap_log_time_);
    wd.setChecked("radioLogTime", use_mcap_log_time_);

    // Filter
    wd.setText("lineEditFilter", filter_text_);

    // Channel table — apply filter and build rows
    auto filtered = filteredChannels();
    std::vector<std::string> headers = {"Channel name", "Schema", "Encoding", "Msg Count"};
    wd.setTableHeaders("tableWidget", headers);

    std::vector<std::vector<std::string>> rows;
    std::vector<int> selected_row_indices;
    rows.reserve(filtered.size());

    for (size_t i = 0; i < filtered.size(); ++i) {
      const auto& ch = *filtered[i];
      rows.push_back({ch.topic, ch.schema, ch.encoding, std::to_string(ch.msg_count)});
      if (selected_topics_.count(ch.topic) > 0) {
        selected_row_indices.push_back(static_cast<int>(i));
      }
    }
    wd.setTableRows("tableWidget", rows);
    wd.setSelectedRows("tableWidget", selected_row_indices);

    // OK enabled only if at least one channel selected
    wd.setOkEnabled(!selected_topics_.empty());

    return wd.toJson();
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (!checked) return false;
    if (widget_name == "radioClamp") { clamp_large_arrays_ = true; return true; }
    if (widget_name == "radioSkip") { clamp_large_arrays_ = false; return true; }
    if (widget_name == "radioPubTime") { use_mcap_log_time_ = false; return true; }
    if (widget_name == "radioLogTime") { use_mcap_log_time_ = true; return true; }
    // Checkbox fires toggled with both true and false
    if (widget_name == "checkBoxUseTimestamp") { use_timestamp_ = checked; return true; }
    return false;
  }

  bool onValueChanged(std::string_view widget_name, int value) override {
    if (widget_name == "spinBox") {
      max_array_size_ = static_cast<unsigned>(value);
      return false;  // no UI refresh needed
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

  bool onSelectionChanged(std::string_view widget_name,
                          const std::vector<std::string>& selected) override {
    if (widget_name == "tableWidget") {
      selected_topics_.clear();
      for (const auto& topic : selected) {
        selected_topics_.insert(topic);
      }
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
    cfg["use_timestamp"] = use_timestamp_;
    cfg["use_mcap_log_time"] = use_mcap_log_time_;
    cfg["selected_topics"] = std::vector<std::string>(selected_topics_.begin(), selected_topics_.end());
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) return false;

    filepath_ = cfg.value("filepath", std::string{});
    max_array_size_ = cfg.value("max_array_size", 500u);
    clamp_large_arrays_ = cfg.value("clamp_large_arrays", true);
    use_timestamp_ = cfg.value("use_timestamp", false);
    use_mcap_log_time_ = cfg.value("use_mcap_log_time", false);

    selected_topics_.clear();
    if (auto it = cfg.find("selected_topics"); it != cfg.end() && it->is_array()) {
      for (const auto& t : *it) {
        if (t.is_string()) selected_topics_.insert(t.get<std::string>());
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

    mcap::McapReader reader;
    auto status = reader.open(filepath_);
    if (!status.ok()) return;

    status = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
    if (!status.ok()) {
      reader.close();
      return;
    }

    // Build message count map from statistics
    std::unordered_map<mcap::ChannelId, uint64_t> msg_counts;
    if (auto stats = reader.statistics()) {
      msg_counts = stats->channelMessageCounts;
    }

    const auto& schemas = reader.schemas();
    for (const auto& [id, channel_ptr] : reader.channels()) {
      ChannelInfo info;
      info.topic = channel_ptr->topic;

      auto schema_it = schemas.find(channel_ptr->schemaId);
      if (schema_it != schemas.end()) {
        info.schema = schema_it->second->name;
        info.encoding = schema_it->second->encoding;
      }

      auto count_it = msg_counts.find(id);
      info.msg_count = (count_it != msg_counts.end()) ? count_it->second : 0;

      all_channels_.push_back(std::move(info));
    }

    // Sort by topic name
    std::sort(all_channels_.begin(), all_channels_.end(),
              [](const ChannelInfo& a, const ChannelInfo& b) { return a.topic < b.topic; });

    reader.close();

    // If no previous selection, select all channels with messages
    if (selected_topics_.empty()) {
      for (const auto& ch : all_channels_) {
        if (ch.msg_count > 0) {
          selected_topics_.insert(ch.topic);
        }
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

    // Split filter by spaces — AND logic (all words must match)
    std::vector<std::string> words;
    std::string word;
    for (char c : filter_text_) {
      if (c == ' ') {
        if (!word.empty()) { words.push_back(word); word.clear(); }
      } else {
        word += c;
      }
    }
    if (!word.empty()) words.push_back(word);

    for (const auto& ch : all_channels_) {
      bool match = true;
      for (const auto& w : words) {
        if (ch.topic.find(w) == std::string::npos) {
          match = false;
          break;
        }
      }
      if (match) result.push_back(&ch);
    }
    return result;
  }

  // Config state
  std::string filepath_;
  unsigned max_array_size_ = 500;
  bool clamp_large_arrays_ = true;
  bool use_timestamp_ = false;
  bool use_mcap_log_time_ = false;
  std::unordered_set<std::string> selected_topics_;
  std::string filter_text_;

  // File analysis results
  std::vector<ChannelInfo> all_channels_;
};

}  // namespace
