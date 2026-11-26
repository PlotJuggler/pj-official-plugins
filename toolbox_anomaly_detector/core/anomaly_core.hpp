// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// The ENGINE layer: the Luau-backed `runAnomalyScript`, delegating to the shared pj_scripting_core
// engine. Only the headless runner links this; the GUI plugin links `anomaly_helpers` alone and the
// host runs its rules.

#pragma once

#include <pj_scripting/marker_engine.h>

#include <pj_base/builtin/plot_markers.hpp>
#include <string>
#include <vector>

#include "anomaly_helpers.hpp"  // re-exported: builtinFunctions / report / rule helpers

namespace anomaly_core {

using SeriesAccessor = PJ::scripting::SeriesView;
using SeriesProvider = PJ::scripting::SeriesProvider;

/// Run a Lua detection rule once over `provider`, returning the emitted markers.
/// On a Lua/compile error returns an empty vector and, if `error` is non-null, sets
/// it to the message (empty on success). The engine also binds a spectral helper:
/// `bandPower(series, fLo, fHi)` returns the summed FFT power in [fLo, fHi] Hz
/// (DC-removed), for vibration detection.
[[nodiscard]] std::vector<PJ::sdk::PlotMarker> runAnomalyScript(
    const std::string& code, const SeriesProvider& provider, std::string* error);

}  // namespace anomaly_core
