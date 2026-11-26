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
// layout: "CLI default", "Custom...", then one row per listed choice
// (kIndexFirstListed + i -> choices[i]). Cheap value type, rebuilt from the
// backend's availableModels() wherever needed rather than cached.
class ModelPicker {
 public:
  explicit ModelPicker(std::vector<ModelChoice> choices) : choices_(std::move(choices)) {}

  static constexpr int kIndexDefault = 0;
  static constexpr int kIndexCustom = 1;
  static constexpr int kIndexFirstListed = 2;

  [[nodiscard]] std::vector<std::string> items() const {
    std::vector<std::string> out = {"CLI default", "Custom..."};
    for (const ModelChoice& c : choices_) {
      out.push_back(c.label);
    }
    return out;
  }

  // Persisted model value -> combo row: empty -> CLI default; a listed id -> its
  // row; anything else (not in this build's catalog) -> Custom.
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

  // Combo row -> value to persist. kIndexCustom or an unknown index -> nullopt,
  // meaning "keep what the custom text box already staged" (onTextChanged runs first).
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
