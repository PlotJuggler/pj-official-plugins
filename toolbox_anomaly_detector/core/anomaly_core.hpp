// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// anomaly_core — the GUI-free heart of the Anomaly Detector. It owns the
// predefined detection-function library and the JSON report/rule helpers, and
// delegates script execution to the SHARED Luau engine (pj_scripting_core's
// runMarkerScript) — the same engine PlotJuggler itself uses for filters, so the
// GUI toolbox and the headless CLI run rules through one Luau VM, not a bundled
// sol2 runtime. Both consumers link this library and call runAnomalyScript(): the
// only difference is where the series come from (the live toolbox host vs. a file
// loaded by the runner) and where the markers go (the ObjectStore vs. a JSON
// report).
//
// No Qt, no plugin host, no GUI. Depends only on pj_base (PlotMarker) +
// pj_scripting_core (which carries Luau + kissfft).

#pragma once

#include <pj_scripting/marker_engine.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <pj_base/builtin/plot_markers.hpp>
#include <string>
#include <vector>

namespace anomaly_core {

/// The detection engine now lives in the shared pj_scripting_core (Luau), linked by
/// both the GUI plugin and the headless runner so a rule runs identically GUI <->
/// headless. These aliases keep the long-standing anomaly_core surface (the series
/// accessor + provider) pointing at the shared engine's types, so callers are
/// unchanged. anomaly_core itself keeps only the GUI-free helpers below: the builtin
/// rule library, the JSON report, and the portable-rule (de)serialization.
using SeriesAccessor = PJ::scripting::SeriesView;
using SeriesProvider = PJ::scripting::SeriesProvider;

/// A named predefined detection function (template Lua). "--SOURCE--" is the
/// placeholder substituted with the selected source-series name.
struct NamedFunction {
  const char* name;
  const char* code;
};

/// The built-in detection-pattern library (shared by the GUI dropdown and the
/// CLI `--list-functions`). Index 0 is the "-- No function --" blank template.
[[nodiscard]] const std::vector<NamedFunction>& builtinFunctions();

/// Replace every "--SOURCE--" in `tmpl` with `source` (no-op if source empty).
[[nodiscard]] std::string substituteSource(std::string tmpl, const std::string& source);

/// Run a Lua detection rule once over `provider`, returning the emitted markers.
/// On a Lua/compile error returns an empty vector and, if `error` is non-null,
/// sets it to the message (empty on success). The engine also binds a spectral
/// helper: `bandPower(series, fLo, fHi)` returns the summed FFT power in the
/// frequency band [fLo, fHi] Hz (DC-removed), for vibration detection.
[[nodiscard]] std::vector<PJ::sdk::PlotMarker> runAnomalyScript(
    const std::string& code, const SeriesProvider& provider, std::string* error);

// ---------------------------------------------------------------------------
// JSON report (structured output for the headless runner / CI pipelines)
// ---------------------------------------------------------------------------

/// Context recorded in the report header + the pass/fail threshold.
struct ReportMeta {
  std::string file;                                                   ///< the data file analysed
  std::string script;                                                 ///< the rule name or path
  std::string source;                                                 ///< the source series
  PJ::sdk::MarkerSeverity fail_on = PJ::sdk::MarkerSeverity::kError;  ///< fail at this severity or above
};

/// Result of serializing a marker set to a JSON report.
struct Report {
  std::string json;     ///< pretty-printed JSON document
  bool failed = false;  ///< true if any marker meets/exceeds fail_on, or has status=fail
  int total = 0;        ///< marker count
};

/// Build a structured JSON report from a marker set: header (file/script/source),
/// overall pass/fail status, per-severity + per-status summary counts, and the full
/// anomaly list (kind, label, severity, status, timestamps, values, color).
[[nodiscard]] Report markersToReport(const std::vector<PJ::sdk::PlotMarker>& markers, const ReportMeta& meta);

// ---------------------------------------------------------------------------
// Portable rule file (the shareable artifact the GUI saves and the runner runs)
// ---------------------------------------------------------------------------

/// A self-contained detection rule. The same JSON document is saved/loaded by the
/// GUI editor and consumed by the headless runner (`--rule`), so a rule authored in
/// the GUI runs unchanged on a server.
struct Rule {
  std::string name;
  std::string description;
  std::string code;    ///< the Lua detection script
  std::string source;  ///< target series ("topic/field"); substituted into "--SOURCE--"
  PJ::sdk::MarkerSeverity fail_on = PJ::sdk::MarkerSeverity::kError;
};

/// Parse a severity name ("info"|"warning"|"error"|"critical"), or nullopt if unknown.
/// The single source of truth for the severity vocabulary (GUI, runner, rule files).
[[nodiscard]] std::optional<PJ::sdk::MarkerSeverity> severityFromString(const std::string& name);

/// Serialize a rule to its portable JSON form:
/// `{"version":1,"name":..,"description":..,"rule":{"code":..,"source":..,"fail_on":".."}}`.
[[nodiscard]] std::string ruleToJson(const Rule& rule);

/// Parse a rule from its JSON form. Returns nullopt + sets `*error` on malformed JSON.
[[nodiscard]] std::optional<Rule> ruleFromJson(const std::string& json, std::string* error);

}  // namespace anomaly_core
