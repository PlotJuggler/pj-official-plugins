// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "claude_sessions.hpp"  // ConversationSummary

namespace assistant_agent::testing {

// The conversation whose id matches, or nullptr. Kept apart from backend_test_helpers.hpp, which
// needs ASSISTANT_CHILD_HELPER on Windows, which the session-store tests do not define.
inline const ConversationSummary* findById(const std::vector<ConversationSummary>& convs, const std::string& id) {
  for (const auto& c : convs) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

}  // namespace assistant_agent::testing
