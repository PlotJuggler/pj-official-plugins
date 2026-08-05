// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// anomaly_helpers — the Luau-FREE part of the Anomaly Detector core: the predefined
// detection-function library, the JSON report, and the portable-rule
// (de)serialization. It depends ONLY on pj_base (PlotMarker types) + nlohmann_json —
// NOT on pj_scripting_core / Luau. The GUI toolbox plugin links THIS (so the plugin
// carries no script engine: it submits rules to the host, which executes them); the
// headless runner links `anomaly_core`, which adds the engine (runAnomalyScript) on
// top of these helpers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <pj_base/builtin/plot_markers.hpp>
#include <string>
#include <vector>

namespace anomaly_core {

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
