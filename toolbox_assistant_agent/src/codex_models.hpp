// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <vector>

#include "model_choice.hpp"

namespace assistant_agent {

// `codexHomeDir()/models_cache.json` (codex_sessions.hpp). Empty when neither CODEX_HOME nor
// HOME resolves.
[[nodiscard]] std::filesystem::path codexModelsCacheFile();

// Reads the Codex CLI's own `models_cache.json` (there is no "list models" command) for the
// settings combo: only `visibility == "list"` entries (a "hide" entry, e.g. an internal review
// model, is not meant to be picked), ordered by `priority` ascending (lower is more prominent
// in Codex's UI). `id` is the `slug`, what `--model` takes; `label` is `display_name` plus
// " — " and `description` when present.
//
// A missing file, invalid JSON or no "models" array yields an empty vector; never throws. The
// combo then offers only "CLI default" and "Custom...".
[[nodiscard]] std::vector<ModelChoice> listCodexModels(const std::filesystem::path& cache_file);

}  // namespace assistant_agent
