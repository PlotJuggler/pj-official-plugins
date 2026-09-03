// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "claude_sessions.hpp"  // ConversationSummary

namespace assistant_agent::testing {

// The conversation whose id matches, or nullptr -- claude_sessions_test.cpp and
// codex_sessions_test.cpp look their listing rows up by id the same way. Kept
// apart from backend_test_helpers.hpp on purpose: that header pulls in the tool
// registry and the SDK test store, and the session-store tests are std-only.
inline const ConversationSummary* findById(const std::vector<ConversationSummary>& convs, const std::string& id) {
  for (const auto& c : convs) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

}  // namespace assistant_agent::testing
