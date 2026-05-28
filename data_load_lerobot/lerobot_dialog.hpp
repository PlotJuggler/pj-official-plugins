#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
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
 public:
  // --- Accessors used by LeRobotSource ---

  const lerobot::DatasetModel* model() const {
    return model_ ? &*model_ : nullptr;
  }
  const std::string& datasetError() const {
    return model_error_;
  }
  /// Episode this LeRobotSource instance imports. Populated when the host
  /// passes a per-instance fanout config carrying `"episode": <int>`.
  std::optional<int64_t> singleEpisode() const {
    return single_episode_;
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
    // Dialog/UI mode: emit the dialog template the user just confirmed AND a
    // top-level `__pj_fanout` array if multi-select produced ≥1 episodes.
    // FileLoader peels `__pj_fanout` to spawn one LeRobotSource per entry
    // (each entry → one DatasetId). The outer fields restore the dialog
    // selection on next open. See FileLoader::extractFanout in pj_app.
    nlohmann::json cfg;
    cfg["filepath"] = filepath_;
    cfg["selected_episodes"] = selected_eps_;

    // Name the dataset tree-root after the dataset folder (e.g. "pusht_v21")
    // rather than the opened file (info.json → "info"). pj_app reads this
    // top-level `display_name` and uses it as the catalog root label (and as
    // the shared prefix combined with each entry's `display_suffix` in fanout).
    // model_->root is the resolved dataset root (walked up to meta/info.json).
    if (model_) {
      const std::string name = model_->root.filename().string();
      if (!name.empty()) {
        cfg["display_name"] = name;
      }
    }

    if (!selected_eps_.empty()) {
      nlohmann::json fanout = nlohmann::json::array();
      for (std::size_t i = 0; i < selected_eps_.size(); ++i) {
        nlohmann::json entry;
        entry["filepath"] = filepath_;
        entry["episode"] = selected_eps_[i];
        entry["display_suffix"] = std::string("ep_") + std::to_string(selected_eps_[i]);
        fanout.push_back(entry.dump());
      }
      cfg["__pj_fanout"] = std::move(fanout);
    }
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    filepath_ = cfg.value("filepath", std::string{});

    // Per-instance fanout mode: the FileLoader passes a sub-config with a
    // single `episode` int. importData reads singleEpisode() to skip the
    // multi-episode iteration. The dialog UI fields stay at defaults — this
    // instance is never shown.
    single_episode_.reset();
    if (auto it = cfg.find("episode"); it != cfg.end() && it->is_number_integer()) {
      single_episode_ = it->get<int64_t>();
    }

    // Restore dialog selection (dialog/UI mode path). Per-instance configs
    // don't carry selected_episodes, so this is a no-op for fanout entries.
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
    if (model_ && single_episode_.has_value()) {
      // Per-instance: verify the episode still exists in the dataset; refuse
      // the config otherwise so the host reports a clean error.
      const int64_t ep = *single_episode_;
      const auto& eps = model_->episodes;
      if (!std::any_of(eps.begin(), eps.end(), [ep](const lerobot::EpisodeInfo& e) { return e.episode_index == ep; })) {
        return false;
      }
    } else if (model_) {
      // Keep only restored indices still present (the dataset may have
      // changed since the layout was saved); empty selection ⇒ default all.
      for (int64_t ep : restored) {
        const auto& eps = model_->episodes;
        if (std::any_of(
                eps.begin(), eps.end(), [ep](const lerobot::EpisodeInfo& e) { return e.episode_index == ep; })) {
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
      std::string item = "ep " + std::to_string(ep.episode_index) + "  -  " + std::to_string(ep.length) + " frames";
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
           std::to_string(model_->episodes.size()) + " episodes  -  cams: " + (cams.empty() ? "(none)" : cams);
  }

  std::string filepath_;
  std::optional<lerobot::DatasetModel> model_;
  std::string model_error_;
  std::vector<std::string> episode_items_;  // aligned 1:1 with model_->episodes
  std::vector<int64_t> selected_eps_;       // episode_index values, ascending
  // Per-instance fanout: set by loadConfig when the host passes a single
  // `episode` int; importData uses it to bypass the multi-episode iteration.
  std::optional<int64_t> single_episode_;
};

}  // namespace
