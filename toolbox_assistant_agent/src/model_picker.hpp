// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "model_choice.hpp"

namespace assistant_agent {

// Maps a backend's model catalog onto (and back from) the settings combo's
// three-section layout: "CLI default", "Custom...", then one row per listed
// choice (kIndexFirstListed + i -> choices[i]). Value type, built fresh from
// a backend's availableModels() wherever it is needed (widget_data()'s
// prefill, onIndexChanged's index -> value mapping) -- a static list or one
// small JSON read on a user click, cheap enough that nothing needs to be
// cached across the two call sites.
class ModelPicker {
 public:
  explicit ModelPicker(std::vector<ModelChoice> choices) : choices_(std::move(choices)) {}

  static constexpr int kIndexDefault = 0;
  static constexpr int kIndexCustom = 1;
  static constexpr int kIndexFirstListed = 2;

  // "CLI default", "Custom...", then each choice's label in order -- ready
  // for wd.setItems().
  [[nodiscard]] std::vector<std::string> items() const {
    std::vector<std::string> out = {"CLI default", "Custom..."};
    for (const ModelChoice& c : choices_) {
      out.push_back(c.label);
    }
    return out;
  }

  // Maps a persisted model value onto a combo row: empty -> CLI default; one
  // of the listed ids -> that row; anything else (unlisted -- a model this
  // build's catalog does not know about, or did not when it was typed) ->
  // Custom.
  [[nodiscard]] int indexForValue(const std::string& persisted) const {
    if (persisted.empty()) {
      return kIndexDefault;
    }
    for (std::size_t i = 0; i < choices_.size(); ++i) {
      if (choices_[i].id == persisted) {
        return kIndexFirstListed + static_cast<int>(i);
      }
    }
    return kIndexCustom;
  }

  // The reverse: a combo row -> the value to persist under the model key.
  // kIndexDefault -> "" (CLI default); a listed row -> its id; kIndexCustom,
  // or an index this build does not recognize -> nullopt, meaning "keep
  // whatever the custom text box already staged" (onTextChanged runs first).
  [[nodiscard]] std::optional<std::string> valueForIndex(int index) const {
    if (index == kIndexDefault) {
      return std::string{};
    }
    if (index >= kIndexFirstListed) {
      const auto i = static_cast<std::size_t>(index - kIndexFirstListed);
      if (i < choices_.size()) {
        return choices_[i].id;
      }
    }
    return std::nullopt;
  }

 private:
  std::vector<ModelChoice> choices_;
};

}  // namespace assistant_agent
