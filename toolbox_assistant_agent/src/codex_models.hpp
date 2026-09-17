// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <vector>

#include "model_choice.hpp"

namespace assistant_agent {

// `${CODEX_HOME:-$HOME/.codex}/models_cache.json` -- the leaf this file adds
// onto codexHomeDir() (codex_sessions.hpp), which is where the env-var
// resolution actually lives. Empty when neither variable resolves a base
// directory.
[[nodiscard]] std::filesystem::path codexModelsCacheFile();

// Reads the Codex CLI's own `models_cache.json` -- the file it maintains for
// itself; there is no separate "list models" command to shell out to -- and
// returns the models worth offering in the settings combo: `visibility ==
// "list"` only (a "hide" entry, e.g. an internal review model, was never
// meant for a human to pick), ordered by `priority` ascending (lower is more
// prominent in Codex's own UI). `id` is the `slug`, exactly what the CLI's
// `--model` flag takes; `label` is `display_name`, plus " — " and
// `description` when the entry has one.
//
// A missing file, invalid JSON, or a shape without a "models" array all yield
// an empty vector -- never throws. This is a convenience list, not a
// contract: an empty result just means the combo falls back to offering
// "CLI default" and "Custom..." only, exactly as if this cache had never
// existed.
[[nodiscard]] std::vector<ModelChoice> listCodexModels(const std::filesystem::path& cache_file);

}  // namespace assistant_agent
