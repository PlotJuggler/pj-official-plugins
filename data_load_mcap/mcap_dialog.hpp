#pragma once

#include <algorithm>
#include <mcap/reader.hpp>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

class McapDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  // --- Accessors for McapSource ---
  const std::string& filepath() const {
    return filepath_;
  }
  unsigned maxArraySize() const {
    return max_array_size_;
  }
  bool clampLargeArrays() const {
    return clamp_large_arrays_;
  }
  /// When true, use the message timestamp if present; fallback to publish time.
  bool useTimestamp() const {
    return use_timestamp_;
  }
  const std::unordered_set<std::string>& selectedTopics() const {
    return selected_topics_;
  }
  const std::string& analyzeError() const {
    return analyze_error_;
  }
  /// Primary encoding for the comboBoxProtocol / parser dialog embedding.
  /// Empty when no channels have been analyzed yet.
  const std::string& primaryEncoding() const {
    return primary_encoding_;
  }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return kMcapManifest;
  }

  std::string ui_content() const override {
    return kDialogMcapUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Array size row
    wd.setRange("spinBox", 0, 9999);
    wd.setValue("spinBox", static_cast<int>(max_array_size_));
    wd.setChecked("radioClamp", clamp_large_arrays_);
    wd.setChecked("radioSkip", !clamp_large_arrays_);

    // Timestamp checkbox
    wd.setChecked("checkBoxUseTimestamp", use_timestamp_);

    // Filter
    wd.setText("lineEditFilter", filter_text_);

    // Channel table — apply filter and build rows
    auto filtered = filteredChannels();
    std::vector<std::string> headers = {"Channel name", "Schema", "Encoding", "Msg Count"};
    wd.setTableHeaders("tableWidget", headers);

    std::vector<std::vector<std::string>> rows;
    std::vector<int> selected_row_indices;
    std::vector<int> disabled_row_indices;
    rows.reserve(filtered.size());

    for (size_t i = 0; i < filtered.size(); ++i) {
      const auto& ch = *filtered[i];
      rows.push_back({ch.topic, ch.schema, ch.encoding, std::to_string(ch.msg_count)});
      if (selected_topics_.count(ch.topic) > 0) {
        selected_row_indices.push_back(static_cast<int>(i));
      }
      if (ch.msg_count == 0) {
        disabled_row_indices.push_back(static_cast<int>(i));
      }
    }
    wd.setTableRows("tableWidget", rows);
    wd.setDisabledRows("tableWidget", disabled_row_indices);
    wd.setSelectedRows("tableWidget", selected_row_indices);

    wd.setShortcut("btnSelectAll", "Ctrl+A");
    wd.setShortcut("btnDeselectAll", "Ctrl+Shift+A");

    // Encoding combobox for pj_parser_slot — hide row when only one encoding
    // (nothing to choose) but keep the widget so DialogEngine can find it.
    wd.setItems("comboBoxProtocol", available_encodings_);
    wd.setCurrentIndex("comboBoxProtocol", encodingIndex(primary_encoding_));
    bool multi_encoding = available_encodings_.size() > 1;
    wd.setVisible("comboBoxProtocol", multi_encoding);
    wd.setVisible("labelEncoding", multi_encoding);

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

  bool onIndexChanged(std::string_view widget_name, int index) override {
    if (widget_name == "comboBoxProtocol") {
      if (index >= 0 && index < static_cast<int>(available_encodings_.size())) {
        primary_encoding_ = available_encodings_[static_cast<size_t>(index)];
      }
      return false;
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxUseTimestamp") {
      use_timestamp_ = checked;
      return true;
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
    use_timestamp_ = cfg.value("use_timestamp", false);

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

    mcap::McapReader reader;
    auto status = reader.open(filepath_);
    if (!status.ok()) {
      return;
    }

    status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    if (!status.ok()) {
      if (status.code == mcap::StatusCode::MissingStatistics) {
        // readSummarySection_ still populated channels and schemas before returning
        // this error — record it so McapSource can route to the error dialog.
        analyze_error_ = "Code: " + std::to_string(static_cast<int>(status.code)) + "\nMessage: " + status.message;
      } else {
        reader.close();
        return;
      }
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
        info.encoding =
            channel_ptr->messageEncoding.empty() ? schema_it->second->encoding : channel_ptr->messageEncoding;
      } else {
        info.encoding = channel_ptr->messageEncoding;
      }

      auto count_it = msg_counts.find(id);
      info.msg_count = (count_it != msg_counts.end()) ? count_it->second : 0;

      all_channels_.push_back(std::move(info));
    }

    // Sort by topic name
    std::sort(all_channels_.begin(), all_channels_.end(), [](const ChannelInfo& a, const ChannelInfo& b) {
      return a.topic < b.topic;
    });

    reader.close();

    // Derive unique encodings (insertion-ordered) for comboBoxProtocol.
    // Preserve the previously selected primary_encoding_ if it is still present.
    {
      std::unordered_set<std::string> seen;
      std::vector<std::string> encodings;
      for (const auto& ch : all_channels_) {
        if (!ch.encoding.empty() && seen.insert(ch.encoding).second) {
          encodings.push_back(ch.encoding);
        }
      }
      available_encodings_ = std::move(encodings);

      bool prev_still_valid = !primary_encoding_.empty() && seen.count(primary_encoding_) > 0;
      if (!prev_still_valid) {
        primary_encoding_ = available_encodings_.empty() ? std::string{} : available_encodings_.front();
      }
    }

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
        if (!word.empty()) {
          words.push_back(word);
          word.clear();
        }
      } else {
        word += c;
      }
    }
    if (!word.empty()) {
      words.push_back(word);
    }

    for (const auto& ch : all_channels_) {
      bool match = true;
      for (const auto& w : words) {
        if (ch.topic.find(w) == std::string::npos) {
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

  int encodingIndex(const std::string& enc) const {
    for (int i = 0; i < static_cast<int>(available_encodings_.size()); ++i) {
      if (available_encodings_[static_cast<size_t>(i)] == enc) {
        return i;
      }
    }
    return 0;
  }

  // Config state
  std::string analyze_error_;
  std::string filepath_;
  unsigned max_array_size_ = 500;
  bool clamp_large_arrays_ = true;
  bool use_timestamp_ = false;
  std::unordered_set<std::string> selected_topics_;
  std::string filter_text_;

  // File analysis results
  std::vector<ChannelInfo> all_channels_;
  std::vector<std::string> available_encodings_;
  std::string primary_encoding_;
};

}  // namespace
