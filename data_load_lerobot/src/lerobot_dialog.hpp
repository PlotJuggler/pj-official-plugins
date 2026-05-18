#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "dataset_model.hpp"

namespace {

// Generated at configure time by pj_embed_ui / pj_embed_manifest.
#include "dialog_lerobot_ui.hpp"
#include "lerobot_manifest.hpp"

/// DataSource-owned dialog: pick which episodes of a LeRobot dataset to load,
/// and how to lay them out on the timeline. Mirrors the ParquetDialog pattern.
class LeRobotDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  // --- Accessors for LeRobotSource ---

  const lerobot::DatasetModel* model() const {
    return model_ ? &*model_ : nullptr;
  }
  const std::string& datasetError() const {
    return model_error_;
  }
  /// Selected episode_index values, ascending (dataset order).
  const std::vector<int64_t>& selectedEpisodes() const {
    return selected_eps_;
  }
  /// Gap inserted between consecutive episodes, in seconds (0 if disabled).
  double gapSeconds() const {
    return separate_episodes_ ? gap_seconds_ : 0.0;
  }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return kLerobotManifest;
  }

  std::string ui_content() const override {
    return kDialogLerobotUi;
  }

  std::string widget_data() override {
    if (!model_ && model_error_.empty() && !filepath_.empty()) {
      loadModel();
    }
    PJ::WidgetData wd;

    if (model_) {
      wd.setLabel("info_label", infoText());
      wd.setListItems("episode_list", episode_items_);
      wd.setSelectedItems("episode_list", selectedItemStrings());
    } else {
      wd.setLabel("info_label", model_error_.empty() ? "No dataset loaded." : model_error_);
      wd.setListItems("episode_list", {});
    }

    wd.setChecked("separate_check", separate_episodes_);
    wd.setValue("gap_spin", gap_seconds_);
    wd.setEnabled("gap_spin", separate_episodes_);
    wd.setOkEnabled("buttonBox", !selected_eps_.empty());
    return wd.toJson();
  }

  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name != "episode_list" || !model_) {
      return false;
    }
    selected_eps_.clear();
    for (std::size_t i = 0; i < episode_items_.size(); ++i) {
      for (const auto& s : selected) {
        if (s == episode_items_[i]) {
          selected_eps_.push_back(model_->episodes[i].episode_index);
          break;
        }
      }
    }
    return true;
  }

  bool onClicked(std::string_view widget_name) override {
    if (!model_) {
      return false;
    }
    if (widget_name == "all_btn") {
      selected_eps_.clear();
      for (const auto& ep : model_->episodes) {
        selected_eps_.push_back(ep.episode_index);
      }
      return true;
    }
    if (widget_name == "none_btn") {
      selected_eps_.clear();
      return true;
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "separate_check") {
      separate_episodes_ = checked;
      return true;
    }
    return false;
  }

  bool onValueChanged(std::string_view widget_name, double value) override {
    if (widget_name == "gap_spin") {
      gap_seconds_ = value;
      return true;
    }
    return false;
  }

  bool onFolderSelected(std::string_view widget_name, std::string_view path) override {
    if (widget_name != "change_btn") {
      return false;
    }
    filepath_ = std::string(path);
    model_.reset();
    model_error_.clear();
    selected_eps_.clear();
    loadModel();
    return true;
  }

  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["filepath"] = filepath_;
    cfg["selected_episodes"] = selected_eps_;
    cfg["separate_episodes"] = separate_episodes_;
    cfg["gap_seconds"] = gap_seconds_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    filepath_ = cfg.value("filepath", std::string{});
    separate_episodes_ = cfg.value("separate_episodes", false);
    gap_seconds_ = cfg.value("gap_seconds", 0.0);
    std::vector<int64_t> restored;
    if (auto it = cfg.find("selected_episodes"); it != cfg.end() && it->is_array()) {
      for (const auto& v : *it) {
        if (v.is_number_integer()) {
          restored.push_back(v.get<int64_t>());
        }
      }
    }
    model_.reset();
    model_error_.clear();
    selected_eps_.clear();
    if (!filepath_.empty()) {
      loadModel();
    }
    if (model_) {
      // Keep only restored indices still present (the dataset may have
      // changed since the layout was saved); empty selection ⇒ default all.
      for (int64_t ep : restored) {
        const auto& eps = model_->episodes;
        if (std::any_of(eps.begin(), eps.end(),
                        [ep](const lerobot::EpisodeInfo& e) { return e.episode_index == ep; })) {
          selected_eps_.push_back(ep);
        }
      }
      if (selected_eps_.empty()) {
        for (const auto& ep : model_->episodes) {
          selected_eps_.push_back(ep.episode_index);
        }
      }
    }
    return true;
  }

 private:
  void loadModel() {
    auto m = lerobot::loadDatasetModel(filepath_);
    if (m) {
      model_ = std::move(*m);
      model_error_.clear();
      rebuildEpisodeItems();
    } else {
      model_.reset();
      model_error_ = m.error();
      episode_items_.clear();
    }
  }

  void rebuildEpisodeItems() {
    episode_items_.clear();
    if (!model_) {
      return;
    }
    for (const auto& ep : model_->episodes) {
      std::string item = "ep " + std::to_string(ep.episode_index) + "  -  " +
                         std::to_string(ep.length) + " frames";
      if (!ep.task_text.empty()) {
        item += "  -  " + ep.task_text;
      }
      episode_items_.push_back(std::move(item));
    }
  }

  std::vector<std::string> selectedItemStrings() const {
    std::vector<std::string> out;
    if (!model_) {
      return out;
    }
    for (std::size_t i = 0; i < model_->episodes.size() && i < episode_items_.size(); ++i) {
      for (int64_t sel : selected_eps_) {
        if (model_->episodes[i].episode_index == sel) {
          out.push_back(episode_items_[i]);
          break;
        }
      }
    }
    return out;
  }

  std::string infoText() const {
    if (!model_) {
      return "No dataset loaded.";
    }
    std::string cams;
    for (const auto& c : model_->camera_names) {
      cams += (cams.empty() ? "" : ", ") + c;
    }
    char fps[32];
    std::snprintf(fps, sizeof(fps), "%g", model_->fps);  // %g: "10", "29.97" — no fake truncation
    return model_->root.string() + "  -  " + model_->codebase_version + "  -  fps=" + fps + "  -  " +
           std::to_string(model_->episodes.size()) + " episodes  -  cams: " +
           (cams.empty() ? "(none)" : cams);
  }

  std::string filepath_;
  std::optional<lerobot::DatasetModel> model_;
  std::string model_error_;
  std::vector<std::string> episode_items_;  // aligned 1:1 with model_->episodes
  std::vector<int64_t> selected_eps_;       // episode_index values, ascending
  bool separate_episodes_ = false;
  double gap_seconds_ = 0.0;
};

}  // namespace
