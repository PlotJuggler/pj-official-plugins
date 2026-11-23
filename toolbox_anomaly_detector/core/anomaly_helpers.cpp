// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "anomaly_helpers.hpp"

#include <algorithm>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string_view>

namespace anomaly_core {

// ---------------------------------------------------------------------------
// Predefined function library
// ---------------------------------------------------------------------------

const std::vector<NamedFunction>& builtinFunctions() {
  static const std::vector<NamedFunction> kFunctions = {
      {"-- No function --",
       "-- Write your own Lua rule.\n"
       "-- series(\"topic/field\"):size() / :at(i) -> {t=.., v=..} / :atTime(t)\n"
       "-- startMarker(t) / closeMarker(t, opts)   createBandMarker(low, high, opts)\n"
       "-- createMarker(x, y, opts): only x = Vline | only y = Hline | both = point\n"
       "--   or: createVerticalMarker(x) / createHorizontalMarker(y) / createPointMarker(x, y)\n"
       "-- when(series, condition):backgroundColor(color) / :event() / :verticalLine()\n"
       "--   conditions: above(x) below(x) between(a, b) outside(a, b) equals(x) isTrue() changes()\n"
       "--   or function(v, t, prev, prev_t) return ... end   (see \"Out of range\", \"Color map\")\n"
       "-- opts (optional table) = {label=.., color=\"#rrggbb\" or a Qt name like \"orange\",\n"
       "--                          severity=\"info|warning|error|critical\"}\n"
       "local s = series(\"--SOURCE--\")\n"},
      {"Showcase (all markers)",
       "-- One of EVERY marker, each with its own colour + label pill.\n"
       "local s = series(\"--SOURCE--\")\n"
       "local n = s:size()\n"
       "local function t(i) return s:at(math.max(0, math.min(n - 1, i))).t end\n"
       "-- Region (blue), from 10% to 22% of the series\n"
       "startMarker(t(math.floor(n * 0.10)))\n"
       "closeMarker(t(math.floor(n * 0.22)), {label=\"region\", color=\"#42a5f5\", severity=\"info\"})\n"
       "-- Vertical line (purple)\n"
       "createVerticalMarker(t(math.floor(n * 0.34)), {label=\"vline\", color=\"#7e57c2\"})\n"
       "-- Point (red) sitting on the real sample\n"
       "local pi = math.floor(n * 0.46)\n"
       "createPointMarker(s:at(pi).t, s:at(pi).v, {label=\"point\", color=\"#e6463c\", severity=\"error\"})\n"
       "-- Horizontal line (teal) at y = 0.8\n"
       "createHorizontalMarker(0.8, {label=\"hline\", color=\"#26a69a\"})\n"
       "-- Value band (orange) over [-0.5, 0.5]\n"
       "createBandMarker(-0.5, 0.5, {label=\"band\", color=\"#ffa726\", severity=\"warning\"})\n"},
      {"Severity colors (lines)",
       "-- Four horizontal lines using the BUILT-IN severity colours (no color override),\n"
       "-- so you can check the info/warning/error/critical palette + label pills.\n"
       "createHorizontalMarker(1.2, {label=\"critical\", severity=\"critical\"})\n"
       "createHorizontalMarker(0.8, {label=\"error\",    severity=\"error\"})\n"
       "createHorizontalMarker(0.4, {label=\"warning\",  severity=\"warning\"})\n"
       "createHorizontalMarker(0.0, {label=\"info\",     severity=\"info\"})\n"},
      {"Threshold (line)",
       "-- A vertical line where the value rises above a threshold.\n"
       "local TH = 0.5\n"
       "when(\"--SOURCE--\", above(TH)):verticalLine({label=\"above \" .. TH})\n"},
      {"Color map (region)",
       "-- Shade the background by the series' value (PlotJuggler 3's ColorMap).\n"
       "-- A color is \"#rrggbb\" or a Qt name. For a discrete state, map values to colors:\n"
       "--   when(\"--SOURCE--\"):backgroundColor({ [0] = \"gray\", [1] = \"green\" })\n"
       "-- or pass your own function(v) returning a color, {color=, label=}, or nil for none.\n"
       "when(\"--SOURCE--\", below(0)):backgroundColor(\"steelblue\")\n"
       "when(\"--SOURCE--\", between(0, 1)):backgroundColor(\"green\")\n"
       "when(\"--SOURCE--\", above(1)):backgroundColor(\"red\")\n"},
      {"Out of range (region)",
       "-- Shade a region while the value is outside [LO, HI].\n"
       "local LO, HI = -0.5, 0.5\n"
       "local opts = {label=\"out of range\", severity=\"warning\"}\n"
       "when(\"--SOURCE--\", outside(LO, HI)):backgroundColor(nil, opts)\n"},
      {"Spike (point)",
       "-- A point event at a sudden jump between consecutive samples.\n"
       "local JUMP = 0.8\n"
       "local function jump(v, t, prev)\n"
       "  return prev ~= nil and math.abs(v - prev) > JUMP\n"
       "end\n"
       "when(\"--SOURCE--\", jump):event({label=\"spike\", severity=\"error\", every=true})\n"},
      {"Incoherent point",
       "-- An isolated outlier: a point far from BOTH of its neighbours.\n"
       "local s = series(\"--SOURCE--\")\n"
       "local DEV = 1.0\n"
       "for i = 1, s:size() - 2 do\n"
       "  local a = s:at(i - 1)\n"
       "  local b = s:at(i)\n"
       "  local c = s:at(i + 1)\n"
       "  if math.abs(b.v - a.v) > DEV and math.abs(b.v - c.v) > DEV then\n"
       "    createPointMarker(b.t, b.v, {label=\"incoherent\", color=\"#aa1ea0\"})\n"
       "  end\n"
       "end\n"},
      {"Flatline (region)",
       "-- Shade a region where the value is stuck (barely changes) for at least MIN_S seconds.\n"
       "local EPS, MIN_S = 1e-4, 0.5\n"
       "local function flat(v, t, prev)\n"
       "  return prev ~= nil and math.abs(v - prev) < EPS\n"
       "end\n"
       "when(\"--SOURCE--\", flat):backgroundColor(nil, {label=\"flat\", minDuration=MIN_S})\n"},
      {"Rate of change (line)",
       "-- A line where the slope (dv/dt, per second) starts to exceed a limit.\n"
       "local RATE = 5.0\n"
       "local function steep(v, t, prev, prev_t)\n"
       "  return prev ~= nil and t > prev_t and math.abs(v - prev) / ((t - prev_t) * 1e-9) > RATE\n"
       "end\n"
       "when(\"--SOURCE--\", steep):verticalLine({label=\"steep\"})\n"},
      {"Limit lines (horizontal)",
       "-- Two horizontal lines at the allowed limits LO and HI.\n"
       "local LO, HI = -0.5, 0.5\n"
       "createHorizontalMarker(HI, {label=\"max\", color=\"#e6463c\"})\n"
       "createHorizontalMarker(LO, {label=\"min\", color=\"#e6463c\"})\n"},
      {"Limit band",
       "-- Draw the allowed value band [LO, HI] as a shaded horizontal band.\n"
       "local LO, HI = -0.5, 0.5\n"
       "createBandMarker(LO, HI, {label=\"allowed\", severity=\"info\"})\n"},
      {"Spectral band power (vibration)",
       "-- Flag excessive vibration energy in a frequency band [F_LO, F_HI] Hz.\n"
       "-- bandPower(series, fLo, fHi) runs an FFT (DC-removed) and sums the band power.\n"
       "local s = series(\"--SOURCE--\")\n"
       "local F_LO, F_HI = 0.5, 1.5\n"
       "local LIMIT = 0.02\n"
       "local p = bandPower(s, F_LO, F_HI)\n"
       "if p > LIMIT and s:size() > 0 then\n"
       "  -- mark the whole record: a region from first to last sample\n"
       "  startMarker(s:at(0).t)\n"
       "  closeMarker(s:at(s:size() - 1).t,\n"
       "    {label=string.format(\"vibration %.3f\", p), severity=\"error\", category=\"spectral\"})\n"
       "end\n"},
      {"Boolean flag (edges)",
       "-- Monitor a boolean/discrete flag series: mark each rising edge (0 -> non-zero)\n"
       "-- with a point, and shade the region while the flag stays raised.\n"
       "local flag = when(\"--SOURCE--\", isTrue())\n"
       "flag:event({label=\"flag\", severity=\"warning\", category=\"flag\"})\n"
       "flag:backgroundColor(nil, {severity=\"warning\", category=\"flag\"})\n"},
  };
  return kFunctions;
}

std::string substituteSource(std::string tmpl, const std::string& source) {
  if (source.empty()) {
    return tmpl;
  }
  const std::string token = "--SOURCE--";
  std::size_t pos = 0;
  while ((pos = tmpl.find(token, pos)) != std::string::npos) {
    tmpl.replace(pos, token.size(), source);
    pos += source.size();
  }
  return tmpl;
}

namespace {

// Length of a Lua long-bracket opener at `pos` ("[[", "[=[", "[==[", ...), or 0 when
// there is none. `level` receives the number of '=' signs.
std::size_t longBracketOpen(const std::string& s, std::size_t pos, std::size_t& level) {
  if (pos >= s.size() || s[pos] != '[') {
    return 0;
  }
  std::size_t i = pos + 1;
  while (i < s.size() && s[i] == '=') {
    ++i;
  }
  if (i >= s.size() || s[i] != '[') {
    return 0;
  }
  level = i - pos - 1;
  return i - pos + 1;
}

// Index just past the closer "]=*]" that matches `level`, or npos when unterminated.
std::size_t longBracketCloseEnd(const std::string& s, std::size_t from, std::size_t level) {
  const std::string closer = "]" + std::string(level, '=') + "]";
  const std::size_t at = s.find(closer, from);
  return at == std::string::npos ? at : at + closer.size();
}

// Index of the quote that closes the Lua short string opening at `s[pos]`, honouring
// backslash escapes; `s.size()` when unterminated. The one definition of "where does this
// string end" for both the comment blanker and the reference scanner.
std::size_t quotedStringEnd(const std::string& s, std::size_t pos) {
  const char quote = s[pos++];
  while (pos < s.size() && s[pos] != quote) {
    pos += (s[pos] == '\\') ? 2 : 1;
  }
  return std::min(pos, s.size());
}

// Lua source with every comment blanked out (spaces, newlines kept) and everything else,
// strings included, byte-for-byte in place. Only comments are removed: the reference
// scanner below then cannot mistake a help comment or a commented-out line for code.
std::string blankLuaComments(const std::string& code) {
  std::string out = code;
  std::size_t i = 0;
  while (i < code.size()) {
    const char c = code[i];
    if (c == '"' || c == '\'') {  // short string
      i = quotedStringEnd(code, i) + 1;
      continue;
    }
    std::size_t level = 0;
    if (const std::size_t open = longBracketOpen(code, i, level); open != 0) {  // long string
      const std::size_t end = longBracketCloseEnd(code, i + open, level);
      i = (end == std::string::npos) ? code.size() : end;
      continue;
    }
    if (c == '-' && i + 1 < code.size() && code[i + 1] == '-') {  // comment
      std::size_t end = 0;
      if (const std::size_t open = longBracketOpen(code, i + 2, level); open != 0) {
        end = longBracketCloseEnd(code, i + 2 + open, level);
        if (end == std::string::npos) {
          end = code.size();
        }
      } else {
        end = code.find('\n', i);
        if (end == std::string::npos) {
          end = code.size();
        }
      }
      for (std::size_t k = i; k < end; ++k) {
        if (out[k] != '\n') {
          out[k] = ' ';
        }
      }
      i = end;
      continue;
    }
    ++i;
  }
  return out;
}

}  // namespace

std::vector<std::string> parseSeriesRefs(const std::string& code) {
  const std::string source = blankLuaComments(code);
  std::vector<std::string> refs;
  // when("name", ...) resolves the series by name inside the host's prelude.
  const std::string_view tokens[] = {"series(", "when("};
  std::size_t pos = 0;
  while (true) {
    std::size_t next = std::string::npos;
    std::size_t token_size = 0;
    for (const std::string_view token : tokens) {
      const std::size_t at = source.find(token, pos);
      if (at < next) {
        next = at;
        token_size = token.size();
      }
    }
    if (next == std::string::npos) {
      break;
    }
    pos = next + token_size;
    while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t')) {
      ++pos;
    }
    if (pos >= source.size() || (source[pos] != '"' && source[pos] != '\'')) {
      continue;  // not a string-literal argument
    }
    const std::size_t start = pos + 1;
    pos = quotedStringEnd(source, pos);
    if (pos > start && pos < source.size()) {
      std::string name = source.substr(start, pos - start);
      if (std::find(refs.begin(), refs.end(), name) == refs.end()) {
        refs.push_back(std::move(name));  // dedup so the host materializes each series once
      }
    }
  }
  return refs;
}

// ---------------------------------------------------------------------------
// JSON report
// ---------------------------------------------------------------------------

namespace {

const char* severityName(PJ::sdk::MarkerSeverity s) {
  switch (s) {
    case PJ::sdk::MarkerSeverity::kInfo:
      return "info";
    case PJ::sdk::MarkerSeverity::kWarning:
      return "warning";
    case PJ::sdk::MarkerSeverity::kError:
      return "error";
    case PJ::sdk::MarkerSeverity::kCritical:
      return "critical";
  }
  return "info";
}

const char* statusName(PJ::sdk::MarkerStatus s) {
  switch (s) {
    case PJ::sdk::MarkerStatus::kNone:
      return "none";
    case PJ::sdk::MarkerStatus::kPass:
      return "pass";
    case PJ::sdk::MarkerStatus::kFail:
      return "fail";
  }
  return "none";
}

const char* kindName(PJ::sdk::MarkerKind k) {
  switch (k) {
    case PJ::sdk::MarkerKind::kRegion:
      return "region";
    case PJ::sdk::MarkerKind::kEvent:
      return "event";
    case PJ::sdk::MarkerKind::kValueBand:
      return "value_band";
    case PJ::sdk::MarkerKind::kLabel:
      return "label";
  }
  return "event";
}

// "#rrggbb" if the marker carries an explicit colour (a != 0), else null (the
// renderer derives the colour from severity).
nlohmann::ordered_json colorField(const PJ::sdk::ColorRGBA& c) {
  if (c.a == 0) {
    return nullptr;
  }
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
  return std::string(buf);
}

}  // namespace

Report markersToReport(const std::vector<PJ::sdk::PlotMarker>& markers, const ReportMeta& meta) {
  using json = nlohmann::ordered_json;

  // Keyed by the canonical names (not enum ordinals) so the summary can't silently
  // skew if the enum is ever reordered.
  json by_sev = {{"info", 0}, {"warning", 0}, {"error", 0}, {"critical", 0}};
  json by_stat = {{"none", 0}, {"pass", 0}, {"fail", 0}};
  bool failed = false;
  const int fail_threshold = static_cast<int>(meta.fail_on);  // ordinal compare is load-bearing here

  json anomalies = json::array();
  for (const PJ::sdk::PlotMarker& m : markers) {
    by_sev[severityName(m.severity)] = by_sev[severityName(m.severity)].get<int>() + 1;
    by_stat[statusName(m.status)] = by_stat[statusName(m.status)].get<int>() + 1;
    if (static_cast<int>(m.severity) >= fail_threshold || m.status == PJ::sdk::MarkerStatus::kFail) {
      failed = true;
    }
    anomalies.push_back({
        {"kind", kindName(m.kind)},
        {"label", m.label},
        {"category", m.category},
        {"description", m.description},
        {"severity", severityName(m.severity)},
        {"status", statusName(m.status)},
        {"t_start_ns", static_cast<std::int64_t>(m.t_start)},
        {"t_end_ns", static_cast<std::int64_t>(m.t_end)},
        {"has_value", m.has_value},
        {"value_low", m.value_low},
        {"value_high", m.value_high},
        {"color", colorField(m.color)},
    });
  }

  json doc = {
      {"file", meta.file},
      {"script", meta.script},
      {"source", meta.source},
      {"status", failed ? "fail" : "pass"},
      {"summary", {{"total", static_cast<int>(markers.size())}, {"by_severity", by_sev}, {"by_status", by_stat}}},
      {"anomalies", anomalies},
  };

  Report report;
  report.json = doc.dump(2);
  report.failed = failed;
  report.total = static_cast<int>(markers.size());
  return report;
}

// ---------------------------------------------------------------------------
// Portable rule file
// ---------------------------------------------------------------------------

std::optional<PJ::sdk::MarkerSeverity> severityFromString(const std::string& name) {
  if (name == "info") {
    return PJ::sdk::MarkerSeverity::kInfo;
  }
  if (name == "warning") {
    return PJ::sdk::MarkerSeverity::kWarning;
  }
  if (name == "error") {
    return PJ::sdk::MarkerSeverity::kError;
  }
  if (name == "critical") {
    return PJ::sdk::MarkerSeverity::kCritical;
  }
  return std::nullopt;
}

std::string ruleToJson(const Rule& rule) {
  const nlohmann::json doc = {
      {"version", 1},
      {"name", rule.name},
      {"description", rule.description},
      {"rule",
       {
           {"code", rule.code},
           {"source", rule.source},
           {"fail_on", severityName(rule.fail_on)},
       }},
  };
  return doc.dump(2);
}

std::optional<Rule> ruleFromJson(const std::string& json, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  const nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    if (error != nullptr) {
      *error = "invalid JSON";
    }
    return std::nullopt;
  }
  Rule rule;
  rule.name = doc.value("name", std::string{});
  rule.description = doc.value("description", std::string{});
  // Accept both the wrapped form ({"rule":{...}}) and a flat {code,source,...}.
  const nlohmann::json& r = (doc.contains("rule") && doc["rule"].is_object()) ? doc["rule"] : doc;
  rule.code = r.value("code", std::string{});
  rule.source = r.value("source", std::string{});
  rule.fail_on = severityFromString(r.value("fail_on", std::string("error"))).value_or(PJ::sdk::MarkerSeverity::kError);
  if (rule.code.empty()) {
    if (error != nullptr) {
      *error = "rule has no 'code'";
    }
    return std::nullopt;
  }
  return rule;
}

std::vector<std::string> ruleInputs(const std::string& code, const std::string& source) {
  std::vector<std::string> refs = parseSeriesRefs(code);
  if (refs.empty() && !source.empty()) {
    refs.push_back(source);
  }
  return refs;
}

}  // namespace anomaly_core
