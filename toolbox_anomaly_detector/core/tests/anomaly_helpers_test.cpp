// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the Luau-FREE half of the Anomaly Detector core. This layer is
// shared verbatim by the GUI toolbox and the headless runner, so a regression here
// shows up in both at once — and unlike the runner's notify_test (gated behind
// PJ_BUILD_ANOMALY_RUNNER), this target builds with the plugin and therefore runs in CI.
//
// What is pinned here is the *contract* the two consumers rely on: the portable rule
// file survives a save/load round-trip, the report's pass/fail verdict and summary
// counts, and the "--SOURCE--" substitution the editor performs on every builtin.

#include <gtest/gtest.h>

#include <anomaly_helpers.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using anomaly_core::Report;
using anomaly_core::ReportMeta;
using anomaly_core::Rule;
using PJ::sdk::MarkerKind;
using PJ::sdk::MarkerSeverity;
using PJ::sdk::MarkerStatus;
using PJ::sdk::PlotMarker;

PlotMarker makeMarker(MarkerSeverity severity, MarkerStatus status) {
  PlotMarker m;
  m.kind = MarkerKind::kEvent;
  m.severity = severity;
  m.status = status;
  m.t_start = 1'000;
  m.label = "m";
  return m;
}

// ---------------------------------------------------------------------------
// Portable rule file — the artifact the GUI saves and the runner consumes
// ---------------------------------------------------------------------------

TEST(RuleJson, RoundTripPreservesEveryField) {
  Rule original;
  original.name = "Spike watch";
  original.description = "Flags sudden jumps";
  original.code = "-- lua\nfor i,p in ipairs(series('a/b')) do end\n";
  original.source = "a/b";
  original.fail_on = MarkerSeverity::kWarning;

  std::string error = "unset";
  const std::optional<Rule> parsed = anomaly_core::ruleFromJson(anomaly_core::ruleToJson(original), &error);

  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(parsed->name, original.name);
  EXPECT_EQ(parsed->description, original.description);
  EXPECT_EQ(parsed->code, original.code);  // newlines and comments survive JSON escaping
  EXPECT_EQ(parsed->source, original.source);
  EXPECT_EQ(parsed->fail_on, original.fail_on);
}

TEST(RuleJson, AcceptsFlatFormWithoutRuleWrapper) {
  // Hand-written rules (and older files) may omit the "rule" wrapper.
  const std::string flat = R"({"code":"x = 1","source":"t/f","fail_on":"critical"})";

  std::string error;
  const std::optional<Rule> parsed = anomaly_core::ruleFromJson(flat, &error);

  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_EQ(parsed->code, "x = 1");
  EXPECT_EQ(parsed->source, "t/f");
  EXPECT_EQ(parsed->fail_on, MarkerSeverity::kCritical);
}

TEST(RuleJson, RejectsMalformedJson) {
  std::string error;
  EXPECT_FALSE(anomaly_core::ruleFromJson("{not json", &error).has_value());
  EXPECT_EQ(error, "invalid JSON");
}

TEST(RuleJson, RejectsRuleWithoutCode) {
  // A rule with no script is unrunnable — the only hard validation error.
  std::string error;
  EXPECT_FALSE(anomaly_core::ruleFromJson(R"({"name":"empty","rule":{"source":"a/b"}})", &error).has_value());
  EXPECT_EQ(error, "rule has no 'code'");
}

TEST(RuleJson, UnknownFailOnFallsBackToErrorWithoutFailing) {
  // Documented lenience: an unrecognized severity degrades to the default rather
  // than rejecting the file, so a rule written by a newer version still loads.
  std::string error;
  const std::optional<Rule> parsed =
      anomaly_core::ruleFromJson(R"({"rule":{"code":"x","fail_on":"catastrophic"}})", &error);

  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_EQ(parsed->fail_on, MarkerSeverity::kError);
}

TEST(Severity, FromStringKnowsExactlyTheFourNames) {
  EXPECT_EQ(anomaly_core::severityFromString("info"), MarkerSeverity::kInfo);
  EXPECT_EQ(anomaly_core::severityFromString("warning"), MarkerSeverity::kWarning);
  EXPECT_EQ(anomaly_core::severityFromString("error"), MarkerSeverity::kError);
  EXPECT_EQ(anomaly_core::severityFromString("critical"), MarkerSeverity::kCritical);
  EXPECT_FALSE(anomaly_core::severityFromString("Error").has_value());  // case-sensitive
  EXPECT_FALSE(anomaly_core::severityFromString("").has_value());
  EXPECT_FALSE(anomaly_core::severityFromString("fatal").has_value());
}

// ---------------------------------------------------------------------------
// "--SOURCE--" substitution — applied to every builtin when the user picks a curve
// ---------------------------------------------------------------------------

TEST(SubstituteSource, ReplacesEveryOccurrenceNotJustTheFirst) {
  const std::string tmpl = "a = series('--SOURCE--'); b = series('--SOURCE--')";
  EXPECT_EQ(anomaly_core::substituteSource(tmpl, "t/f"), "a = series('t/f'); b = series('t/f')");
}

TEST(SubstituteSource, EmptySourceLeavesTheTemplateUntouched) {
  // The editor opens with no curve selected; the placeholder must stay visible.
  const std::string tmpl = "series('--SOURCE--')";
  EXPECT_EQ(anomaly_core::substituteSource(tmpl, ""), tmpl);
}

TEST(SubstituteSource, DoesNotRescanInsertedTextAndTerminates) {
  // A series literally named like the token must not send the replace loop
  // spinning over its own output.
  EXPECT_EQ(anomaly_core::substituteSource("x=--SOURCE--", "--SOURCE--"), "x=--SOURCE--");
}

// ---------------------------------------------------------------------------
// JSON report — the CI-facing artifact (verdict + summary + anomaly list)
// ---------------------------------------------------------------------------

TEST(MarkersToReport, CountsBySeverityAndStatus) {
  const std::vector<PlotMarker> markers{
      makeMarker(MarkerSeverity::kInfo, MarkerStatus::kNone),
      makeMarker(MarkerSeverity::kInfo, MarkerStatus::kPass),
      makeMarker(MarkerSeverity::kWarning, MarkerStatus::kNone),
  };
  ReportMeta meta;
  meta.file = "demo.csv";
  meta.script = "Spike";
  meta.source = "a/b";
  meta.fail_on = MarkerSeverity::kCritical;

  const Report report = anomaly_core::markersToReport(markers, meta);
  const nlohmann::json doc = nlohmann::json::parse(report.json);

  EXPECT_EQ(report.total, 3);
  EXPECT_FALSE(report.failed);
  EXPECT_EQ(doc["status"], "pass");
  EXPECT_EQ(doc["file"], "demo.csv");
  EXPECT_EQ(doc["script"], "Spike");
  EXPECT_EQ(doc["source"], "a/b");
  EXPECT_EQ(doc["summary"]["total"], 3);
  EXPECT_EQ(doc["summary"]["by_severity"]["info"], 2);
  EXPECT_EQ(doc["summary"]["by_severity"]["warning"], 1);
  EXPECT_EQ(doc["summary"]["by_severity"]["error"], 0);
  EXPECT_EQ(doc["summary"]["by_severity"]["critical"], 0);
  EXPECT_EQ(doc["summary"]["by_status"]["none"], 2);
  EXPECT_EQ(doc["summary"]["by_status"]["pass"], 1);
  EXPECT_EQ(doc["summary"]["by_status"]["fail"], 0);
  EXPECT_EQ(doc["anomalies"].size(), 3u);
}

TEST(MarkersToReport, FailsOnlyAtOrAboveTheThreshold) {
  const std::vector<PlotMarker> markers{makeMarker(MarkerSeverity::kError, MarkerStatus::kNone)};

  ReportMeta below;
  below.fail_on = MarkerSeverity::kCritical;
  EXPECT_FALSE(anomaly_core::markersToReport(markers, below).failed);

  ReportMeta at;
  at.fail_on = MarkerSeverity::kError;
  EXPECT_TRUE(anomaly_core::markersToReport(markers, at).failed);

  ReportMeta above;
  above.fail_on = MarkerSeverity::kInfo;
  EXPECT_TRUE(anomaly_core::markersToReport(markers, above).failed);
}

TEST(MarkersToReport, ExplicitFailStatusOverridesTheThreshold) {
  // A rule that asserts a verdict fails the run even at the lowest severity.
  const std::vector<PlotMarker> markers{makeMarker(MarkerSeverity::kInfo, MarkerStatus::kFail)};
  ReportMeta meta;
  meta.fail_on = MarkerSeverity::kCritical;

  const Report report = anomaly_core::markersToReport(markers, meta);
  EXPECT_TRUE(report.failed);
  EXPECT_EQ(nlohmann::json::parse(report.json)["status"], "fail");
}

TEST(MarkersToReport, ColorIsNullUnlessTheMarkerSetsAlpha) {
  PlotMarker uncolored = makeMarker(MarkerSeverity::kInfo, MarkerStatus::kNone);
  PlotMarker colored = makeMarker(MarkerSeverity::kInfo, MarkerStatus::kNone);
  colored.color = {0x12, 0x34, 0x56, 0xff};

  const std::vector<PlotMarker> markers{uncolored, colored};
  const nlohmann::json doc = nlohmann::json::parse(anomaly_core::markersToReport(markers, ReportMeta{}).json);

  EXPECT_TRUE(doc["anomalies"][0]["color"].is_null());  // renderer derives it from severity
  EXPECT_EQ(doc["anomalies"][1]["color"], "#123456");
}

TEST(MarkersToReport, EmptyMarkerSetPasses) {
  const Report report = anomaly_core::markersToReport(std::vector<PlotMarker>{}, ReportMeta{});
  EXPECT_EQ(report.total, 0);
  EXPECT_FALSE(report.failed);
  EXPECT_EQ(nlohmann::json::parse(report.json)["status"], "pass");
}

// ---------------------------------------------------------------------------
// Builtin library — shared by the GUI dropdown and `--list-functions`
// ---------------------------------------------------------------------------

TEST(Builtins, AreWellFormedAndUniquelyNamed) {
  const std::vector<anomaly_core::NamedFunction>& builtins = anomaly_core::builtinFunctions();
  ASSERT_FALSE(builtins.empty());

  // Index 0 is the blank template the editor starts on.
  EXPECT_STREQ(builtins[0].name, "-- No function --");

  std::set<std::string> names;
  for (const anomaly_core::NamedFunction& fn : builtins) {
    ASSERT_NE(fn.name, nullptr);
    ASSERT_NE(fn.code, nullptr);
    EXPECT_FALSE(std::string(fn.name).empty());
    // Duplicate names would make the runner's `--script <name>` lookup ambiguous.
    EXPECT_TRUE(names.insert(fn.name).second) << "duplicate builtin name: " << fn.name;
  }
}

}  // namespace
