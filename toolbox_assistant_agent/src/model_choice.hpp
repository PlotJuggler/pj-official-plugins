// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace assistant_agent {

// One entry in a backend's model picker. `id` is the CLI's `--model` value; `label` is what
// the settings combo shows.
struct ModelChoice {
  std::string id;
  std::string label;
};

}  // namespace assistant_agent
