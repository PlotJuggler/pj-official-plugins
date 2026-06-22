// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "anomaly_core.hpp"

namespace anomaly_core {

// runAnomalyScript — delegates to the shared Luau engine (pj_scripting_core), so a
// rule runs identically here and in the headless runner. The Lua-free helpers
// (builtin library, JSON report, rule (de)serialization) live in anomaly_helpers.
std::vector<PJ::sdk::PlotMarker> runAnomalyScript(
    const std::string& code, const SeriesProvider& provider, std::string* error) {
  return PJ::scripting::runMarkerScript(code, provider, error);
}

}  // namespace anomaly_core
