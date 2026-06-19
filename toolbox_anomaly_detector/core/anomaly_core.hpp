// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// anomaly_core — the GUI-free heart of the Anomaly Detector. It owns the sol2 Lua
// runtime, the read-only series accessor, the marker-emitting primitives, and the
// predefined detection-function library. Both the GUI toolbox plugin and the
// headless CLI runner link this library and run the SAME script through
// runAnomalyScript(): the only difference is where the series come from (the live
// toolbox host vs. a file loaded by the runner) and where the markers go (the
// ObjectStore vs. a JSON report).
//
// No Qt, no plugin host, no GUI. Depends only on pj_base (PlotMarker) + sol2.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <pj_base/builtin/plot_markers.hpp>
#include <string>
#include <vector>

namespace anomaly_core {

/// One sample: timestamp in nanoseconds (as double) and value.
struct TimePoint {
  double t;
  double v;
};

/// Read-only access to a float series' samples. Timestamps are nanoseconds.
struct SeriesAccessor {
  std::vector<double> timestamps;
  std::vector<double> values;

  [[nodiscard]] std::size_t size() const;
  /// Sample at index, or nullopt if out of range.
  [[nodiscard]] std::optional<TimePoint> at(std::size_t index) const;
  /// Linearly interpolated value at time t (clamped to the endpoints).
  [[nodiscard]] double atTime(double t) const;
};

/// How the Lua `series("name")` accessor resolves names to data. The runner and
/// the plugin each supply their own backing store; the engine stays agnostic.
struct SeriesProvider {
  std::vector<std::string> names;                                ///< for GetSeriesNames()
  std::function<const SeriesAccessor*(const std::string&)> get;  ///< name -> series or nullptr
};

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
