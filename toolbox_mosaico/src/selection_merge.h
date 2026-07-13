// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace mosaico {

// Merge the selection reported by the dialog host with the previously selected
// names. The host only reports rows currently visible under the view filter,
// so a previously selected name the filter hides must be kept explicitly —
// otherwise picking a topic while the filter conceals another would silently
// drop the hidden one (same contract as the ros2/foxglove pickers). A visible
// name missing from `reported` was deselected by the user and is dropped.
// `known` bounds the result to names present in the current listing;
// duplicates are dropped.
inline std::vector<std::string> mergeReportedSelection(
    const std::vector<std::string>& previous, const std::vector<std::string>& reported,
    const std::vector<std::string>& known, const std::function<bool(const std::string&)>& hidden) {
  auto contains = [](const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
  };
  std::vector<std::string> next;
  for (const std::string& name : previous) {
    if (contains(known, name) && hidden(name) && !contains(next, name)) {
      next.push_back(name);
    }
  }
  for (const std::string& name : reported) {
    if (contains(known, name) && !contains(next, name)) {
      next.push_back(name);
    }
  }
  return next;
}

}  // namespace mosaico
