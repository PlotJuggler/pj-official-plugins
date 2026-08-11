// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <iomanip>
#include <sstream>
#include <string>

#include "turn_metrics.hpp"

namespace assistant_agent {

// What the last turn moved, and the one line the user reads about it.
//
// The panel shows `ChatSession::statusText()` followed by whatever summary()
// returns, so an empty summary leaves the status line exactly as it was. That
// is the correct answer for a backend that reports no usage at all (Ollama runs
// locally; Echo is not a model), which is why nothing here special-cases a
// backend: a turn that reports nothing simply never reaches record().
//
// Pure and Qt-free, like ChatSession and SeriesStats — unit-tested in
// tests/usage_ledger_test.cpp.
class UsageLedger {
 public:
  // Fold one finished turn in. Turns that produced no result record arrive
  // invalid and must be ignored: a failed turn has nothing to report, and
  // blanking the figures would flicker the label back to a bare "Ready" for no
  // reason the user can see.
  void record(const TurnMetrics& m) {
    if (!m.valid) {
      return;
    }
    last_ = m;
  }

  // Start a new conversation: the "New chat" button calls this alongside
  // ChatSession::clear(), so the figures follow the transcript they belong to.
  void reset() {
    *this = UsageLedger{};
  }

  [[nodiscard]] bool empty() const {
    return !last_.valid;
  }

  // Everything that went up the wire for one turn: the fresh prompt plus the
  // history replayed from cache.
  [[nodiscard]] static long long sentTokens(const TurnMetrics& m) {
    return static_cast<long long>(m.input_tokens) + m.cache_read_tokens + m.cache_creation_tokens;
  }

  // Appended to the status label after every turn, so it is read more often
  // than anything else in the panel.
  //
  // "Sent" is all three input counters added up, not `input_tokens`. In a real
  // --resume turn `input_tokens` is around 10 while `cache_read_tokens` is
  // fifteen thousand: the conversation IS being sent every turn, it is just
  // served from cache. Reporting the uncached remainder alone would show "10"
  // for a 14.5k prompt — a true number that tells the user something false.
  [[nodiscard]] std::string summary() const {
    if (empty()) {
      return {};
    }
    return " - ↑" + formatTokens(sentTokens(last_)) + " ↓" + formatTokens(last_.output_tokens);
  }

 private:
  // Thousands past 1k, one decimal — "14.5k" reads at a glance where "14508"
  // has to be counted. Below that the exact figure is short enough to keep.
  static std::string formatTokens(long long n) {
    if (n < 1000) {
      return std::to_string(n);
    }
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << (static_cast<double>(n) / 1000.0) << "k";
    return os.str();
  }

  TurnMetrics last_;
};

}  // namespace assistant_agent
