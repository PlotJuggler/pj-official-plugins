// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// anomaly_core — the ENGINE layer. It adds the Luau-backed `runAnomalyScript` (which
// delegates to the SHARED pj_scripting_core engine) on top of the Lua-free
// `anomaly_helpers`. Links pj_scripting_core (Luau + kissfft).
//
// The HEADLESS runner links this (it executes rules locally, no PlotJuggler host).
// The GUI toolbox plugin links only `anomaly_helpers` — it submits rules to the host,
// which executes them — so the plugin carries no script engine. Both paths run rules
// through the same engine, so a rule behaves identically GUI <-> headless.

#pragma once

#include <pj_scripting/marker_engine.h>

#include <pj_base/builtin/plot_markers.hpp>
#include <string>
#include <vector>

#include "anomaly_helpers.hpp"  // re-exported: builtinFunctions / report / rule helpers

namespace anomaly_core {

/// The shared engine's series types, kept under the long-standing anomaly_core names
/// so the runner's call sites are unchanged.
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
