// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "anomaly_core.hpp"

namespace anomaly_core {

std::vector<PJ::sdk::PlotMarker> runAnomalyScript(
    const std::string& code, const SeriesProvider& provider, std::string* error) {
  return PJ::scripting::runMarkerScript(code, provider, error);
}

}  // namespace anomaly_core
