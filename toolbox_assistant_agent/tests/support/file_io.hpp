// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <fstream>
#include <sstream>
#include <string>

namespace assistant_agent::testing {

// The whole file as bytes; empty when it cannot be opened.
inline std::string readFileToString(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace assistant_agent::testing
