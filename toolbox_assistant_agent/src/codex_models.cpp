// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "codex_models.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

#include "codex_sessions.hpp"  // codexHomeDir

namespace assistant_agent {

std::filesystem::path codexModelsCacheFile() {
  const std::filesystem::path base = codexHomeDir();
  return base.empty() ? std::filesystem::path{} : base / "models_cache.json";
}

std::vector<ModelChoice> listCodexModels(const std::filesystem::path& cache_file) {
  std::vector<ModelChoice> out;
  if (cache_file.empty()) {
    return out;
  }
  std::ifstream file(cache_file);
  if (!file) {
    return out;
  }
  const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) {
    return out;
  }
  const auto& models = doc.value("models", nlohmann::json::array());
  if (!models.is_array()) {
    return out;
  }

  // priority + choice, sorted below -- keeping them paired is simpler than a
  // parallel-array sort.
  std::vector<std::pair<int, ModelChoice>> entries;
  for (const auto& m : models) {
    if (!m.is_object() || m.value("visibility", std::string{}) != "list") {
      continue;
    }
    const std::string slug = m.value("slug", std::string{});
    if (slug.empty()) {
      continue;
    }
    const std::string display_name = m.value("display_name", slug);
    const std::string description = m.value("description", std::string{});
    std::string label = display_name;
    if (!description.empty()) {
      label += " — " + description;
    }
    entries.emplace_back(m.value("priority", 0), ModelChoice{slug, label});
  }
  std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  out.reserve(entries.size());
  for (auto& entry : entries) {
    out.push_back(std::move(entry.second));
  }
  return out;
}

}  // namespace assistant_agent
