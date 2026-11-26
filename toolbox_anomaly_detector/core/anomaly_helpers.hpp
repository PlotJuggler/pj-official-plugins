// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// The Luau-free layer: builtin rule library, JSON report and portable-rule (de)serialization.
// Depends only on pj_base and nlohmann_json, so the GUI plugin carries no script engine.

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

/// The series a rule reads: the literal first argument of every `series("...")` and
/// `when("...", ...)` call in `code` (either quote), in order of first appearance, deduplicated.
/// Lua comments (`--`, `--[[ ]]`, `--[==[ ]==]`) are skipped; a `--` inside a string is not a comment.
///
/// The GUI declares these as generator inputs. The host materializes EVERY declared input
/// whole-series on each run, so declaring only what the rule touches keeps the preview from
/// re-materializing the dataset. Dynamic names (`series("x"..v)`) are not seen.
[[nodiscard]] std::vector<std::string> parseSeriesRefs(const std::string& code);

/// The generator inputs the GUI declares for `code`: its series refs, or `source` when it reads none (a rule of fixed
/// lines or bands). The host picks the dataset a Timeseries or Dataset rule belongs to from its inputs and refuses a
/// rule that has none; pass an empty `source` for a Global rule, which needs no input.
[[nodiscard]] std::vector<std::string> ruleInputs(const std::string& code, const std::string& source);

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

/// A self-contained detection rule; its JSON form is shared by the GUI editor and the runner's `--rule`.
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
