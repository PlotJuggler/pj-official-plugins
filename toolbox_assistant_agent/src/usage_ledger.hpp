// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <iomanip>
#include <sstream>
#include <string>

#include "turn_metrics.hpp"

namespace assistant_agent {

// Token usage of the last turn, shown after `ChatSession::statusText()`. An empty
// summary() leaves the status line as it was, which is right for a backend that
// reports no usage. Pure and Qt-free.
class UsageLedger {
 public:
  // Invalid metrics (a turn with no result record) are ignored: blanking the
  // figures would flicker the label back to a bare "Ready".
  void record(const TurnMetrics& m) {
    if (!m.valid) {
      return;
    }
    last_ = m;
  }

  void reset() {
    *this = UsageLedger{};
  }

  [[nodiscard]] bool empty() const {
    return !last_.valid;
  }

  // Fresh prompt plus history replayed from cache.
  [[nodiscard]] static long long sentTokens(const TurnMetrics& m) {
    return static_cast<long long>(m.input_tokens) + m.cache_read_tokens + m.cache_creation_tokens;
  }

  // "Sent" sums all three input counters, not `input_tokens`: in a --resume turn
  // `input_tokens` is ~10 while `cache_read_tokens` is ~15000, so the uncached
  // remainder alone would show "10" for a 14.5k prompt.
  [[nodiscard]] std::string summary() const {
    if (empty()) {
      return {};
    }
    return " - ↑" + formatTokens(sentTokens(last_)) + " ↓" + formatTokens(last_.output_tokens);
  }

 private:
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
