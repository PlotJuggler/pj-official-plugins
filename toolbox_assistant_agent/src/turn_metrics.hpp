// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

namespace assistant_agent {

// What one turn cost, as reported by the CLI in its final record. Its own header so the
// backend seam (llm_backend.hpp) does not pull in the stream-json parser.
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
