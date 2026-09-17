// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

namespace assistant_agent {

// What one turn cost, as reported by the CLI itself in its final record. Worth
// having beyond benchmarking: this is the only place the real price and token
// split of a turn is visible, and the cache figures explain why even a trivial
// turn is not free (the CLI's own system prompt is re-established each time).
//
// Lives in its own header rather than next to the parser that fills it, because
// the backend seam (llm_backend.hpp) carries these to the panel and must not
// drag the Claude CLI's stream-json reader along with them.
struct TurnMetrics {
  double cost_usd = 0.0;
  int input_tokens = 0;
  int output_tokens = 0;
  int cache_read_tokens = 0;
  int cache_creation_tokens = 0;
  int api_ms = 0;  // time the CLI spent talking to the API, excluding its own startup
  bool valid = false;
};

}  // namespace assistant_agent
