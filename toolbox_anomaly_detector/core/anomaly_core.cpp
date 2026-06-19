// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "anomaly_core.hpp"

#include <kissfft/kiss_fftr.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

namespace anomaly_core {

// ---------------------------------------------------------------------------
// SeriesAccessor
// ---------------------------------------------------------------------------

std::size_t SeriesAccessor::size() const {
  return timestamps.size();
}

std::optional<TimePoint> SeriesAccessor::at(std::size_t index) const {
  if (index >= timestamps.size()) {
    return std::nullopt;
  }
  return TimePoint{timestamps[index], values[index]};
}

double SeriesAccessor::atTime(double t) const {
  if (timestamps.empty()) {
    return 0.0;
  }
  auto it = std::lower_bound(timestamps.begin(), timestamps.end(), t);
  if (it == timestamps.end()) {
    return values.back();
  }
  if (it == timestamps.begin()) {
    return values.front();
  }
  const std::size_t idx = static_cast<std::size_t>(it - timestamps.begin());
  const double t0 = timestamps[idx - 1];
  const double t1 = timestamps[idx];
  const double v0 = values[idx - 1];
  const double v1 = values[idx];
  if (t1 == t0) {
    return v0;
  }
  return v0 + (t - t0) / (t1 - t0) * (v1 - v0);
}

// ---------------------------------------------------------------------------
// Predefined function library
// ---------------------------------------------------------------------------

const std::vector<NamedFunction>& builtinFunctions() {
  static const std::vector<NamedFunction> kFunctions = {
      {"-- No function --",
       "-- Write your own Lua rule.\n"
       "-- series(\"topic/field\"):size() / :at(i) -> {t=.., v=..} / :atTime(t)\n"
       "-- startMarker(t) / closeMarker(t, opts)   createDataEvent(low, high, opts)\n"
       "-- createEvent(x, y, opts): only x = Vline | only y = Hline | both = point\n"
       "-- opts (optional table) = {label=.., color=\"#rrggbb\", severity=\"info|warning|error|critical\"}\n"
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
       "createEvent(t(math.floor(n * 0.34)), nil, {label=\"vline\", color=\"#7e57c2\"})\n"
       "-- Point (red) sitting on the real sample\n"
       "local pi = math.floor(n * 0.46)\n"
       "createEvent(s:at(pi).t, s:at(pi).v, {label=\"point\", color=\"#e6463c\", severity=\"error\"})\n"
       "-- Horizontal line (teal) at y = 0.8\n"
       "createEvent(nil, 0.8, {label=\"hline\", color=\"#26a69a\"})\n"
       "-- Value band (orange) over [-0.5, 0.5]\n"
       "createDataEvent(-0.5, 0.5, {label=\"band\", color=\"#ffa726\", severity=\"warning\"})\n"},
      {"Severity colors (lines)",
       "-- Four horizontal lines using the BUILT-IN severity colours (no color override),\n"
       "-- so you can check the info/warning/error/critical palette + label pills.\n"
       "createEvent(nil, 1.2, {label=\"critical\", severity=\"critical\"})\n"
       "createEvent(nil, 0.8, {label=\"error\",    severity=\"error\"})\n"
       "createEvent(nil, 0.4, {label=\"warning\",  severity=\"warning\"})\n"
       "createEvent(nil, 0.0, {label=\"info\",     severity=\"info\"})\n"},
      {"Threshold (line)",
       "-- A vertical line at every point above a threshold.\n"
       "local s = series(\"--SOURCE--\")\n"
       "local TH = 0.5\n"
       "for i = 0, s:size() - 1 do\n"
       "  local p = s:at(i)\n"
       "  if p.v > TH then createEvent(p.t) end\n"
       "end\n"},
      {"Out of range (region)",
       "-- Shade a region while the value is outside [LO, HI].\n"
       "local s = series(\"--SOURCE--\")\n"
       "local LO, HI = -0.5, 0.5\n"
       "local open = false\n"
       "for i = 0, s:size() - 1 do\n"
       "  local p = s:at(i)\n"
       "  local bad = p.v < LO or p.v > HI\n"
       "  if bad and not open then startMarker(p.t); open = true\n"
       "  elseif (not bad) and open then closeMarker(p.t); open = false end\n"
       "end\n"
       "if open and s:size() > 0 then closeMarker(s:at(s:size() - 1).t) end\n"},
      {"Spike (point)",
       "-- A point event at a sudden jump between consecutive samples.\n"
       "local s = series(\"--SOURCE--\")\n"
       "local JUMP = 0.8\n"
       "for i = 1, s:size() - 1 do\n"
       "  local a = s:at(i - 1)\n"
       "  local b = s:at(i)\n"
       "  if math.abs(b.v - a.v) > JUMP then\n"
       "    createEvent(b.t, b.v, {label=\"spike\", severity=\"error\"})\n"
       "  end\n"
       "end\n"},
      {"Incoherent point",
       "-- An isolated outlier: a point far from BOTH of its neighbours.\n"
       "local s = series(\"--SOURCE--\")\n"
       "local DEV = 1.0\n"
       "for i = 1, s:size() - 2 do\n"
       "  local a = s:at(i - 1)\n"
       "  local b = s:at(i)\n"
       "  local c = s:at(i + 1)\n"
       "  if math.abs(b.v - a.v) > DEV and math.abs(b.v - c.v) > DEV then\n"
       "    createEvent(b.t, b.v, {label=\"incoherent\", color=\"#aa1ea0\"})\n"
       "  end\n"
       "end\n"},
      {"Flatline (region)",
       "-- Shade a region where the value is stuck (barely changes).\n"
       "local s = series(\"--SOURCE--\")\n"
       "local EPS = 1e-4\n"
       "local open = false\n"
       "for i = 1, s:size() - 1 do\n"
       "  local a = s:at(i - 1)\n"
       "  local b = s:at(i)\n"
       "  local flat = math.abs(b.v - a.v) < EPS\n"
       "  if flat and not open then startMarker(a.t); open = true\n"
       "  elseif (not flat) and open then closeMarker(a.t); open = false end\n"
       "end\n"
       "if open and s:size() > 0 then closeMarker(s:at(s:size() - 1).t) end\n"},
      {"Rate of change (line)",
       "-- A line where the slope (dv/dt) exceeds a limit.\n"
       "local s = series(\"--SOURCE--\")\n"
       "local RATE = 5.0\n"
       "for i = 1, s:size() - 1 do\n"
       "  local a = s:at(i - 1)\n"
       "  local b = s:at(i)\n"
       "  local dt = (b.t - a.t) * 1e-9\n"
       "  if dt > 0 and math.abs(b.v - a.v) / dt > RATE then createEvent(b.t) end\n"
       "end\n"},
      {"Limit lines (horizontal)",
       "-- Two horizontal lines at the allowed limits LO and HI.\n"
       "local LO, HI = -0.5, 0.5\n"
       "createEvent(nil, HI, {label=\"max\", color=\"#e6463c\"})\n"
       "createEvent(nil, LO, {label=\"min\", color=\"#e6463c\"})\n"},
      {"Limit band",
       "-- Draw the allowed value band [LO, HI] as a shaded horizontal band.\n"
       "local LO, HI = -0.5, 0.5\n"
       "createDataEvent(LO, HI, {label=\"allowed\", severity=\"info\"})\n"},
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
       "local s = series(\"--SOURCE--\")\n"
       "local raised = false\n"
       "for i = 0, s:size() - 1 do\n"
       "  local p = s:at(i)\n"
       "  local on = p.v ~= 0\n"
       "  if on and not raised then\n"
       "    createEvent(p.t, p.v, {label=\"flag\", severity=\"warning\", category=\"flag\"})\n"
       "    startMarker(p.t); raised = true\n"
       "  elseif (not on) and raised then\n"
       "    closeMarker(p.t, {severity=\"warning\", category=\"flag\"}); raised = false\n"
       "  end\n"
       "end\n"
       "if raised and s:size() > 0 then closeMarker(s:at(s:size() - 1).t, {severity=\"warning\"}) end\n"},
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

// ---------------------------------------------------------------------------
// Optional marker fields (opts table) — label / color / severity / ...
// ---------------------------------------------------------------------------

namespace {

// Parse "#rrggbb" (or "rrggbb") into an opaque ColorRGBA (a=255 -> renderer uses it
// as an override instead of deriving the color from severity).
std::optional<PJ::sdk::ColorRGBA> parseHexColor(std::string h) {
  if (!h.empty() && h.front() == '#') {
    h.erase(0, 1);
  }
  if (h.size() != 6) {
    return std::nullopt;
  }
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  int comp[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    const int hi = nibble(h[static_cast<std::size_t>(i) * 2]);
    const int lo = nibble(h[static_cast<std::size_t>(i) * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    comp[i] = hi * 16 + lo;
  }
  return PJ::sdk::ColorRGBA{
      static_cast<uint8_t>(comp[0]), static_cast<uint8_t>(comp[1]), static_cast<uint8_t>(comp[2]), 255};
}

PJ::sdk::MarkerSeverity parseSeverity(const std::string& s) {
  return severityFromString(s).value_or(PJ::sdk::MarkerSeverity::kInfo);
}

PJ::sdk::MarkerStatus parseStatus(const std::string& s) {
  if (s == "pass") {
    return PJ::sdk::MarkerStatus::kPass;
  }
  if (s == "fail") {
    return PJ::sdk::MarkerStatus::kFail;
  }
  return PJ::sdk::MarkerStatus::kNone;
}

// Apply an optional Lua opts table { label, description, category, color, severity,
// status } onto a marker. Unknown / missing keys are left at the marker's default.
void applyOpts(PJ::sdk::PlotMarker& m, const sol::optional<sol::table>& opts) {
  if (!opts) {
    return;
  }
  const sol::table& t = *opts;
  if (const sol::optional<std::string> label = t["label"]) {
    m.label = *label;
  }
  if (const sol::optional<std::string> desc = t["description"]) {
    m.description = *desc;
  }
  if (const sol::optional<std::string> cat = t["category"]) {
    m.category = *cat;
  }
  if (const sol::optional<std::string> color = t["color"]) {
    if (const auto rgba = parseHexColor(*color)) {
      m.color = *rgba;
    }
  }
  if (const sol::optional<std::string> sev = t["severity"]) {
    m.severity = parseSeverity(*sev);
  }
  if (const sol::optional<std::string> st = t["status"]) {
    m.status = parseStatus(*st);
  }
}

// Summed FFT power in the band [fLo, fHi] Hz, DC-removed. Sampling interval is
// derived from the series timestamps (assumed ~uniform). Returns 0 for series too
// short or with non-positive dt. Mirrors toolbox_fft's real-FFT path (kissfft).
double computeBandPower(const SeriesAccessor& sa, double fLo, double fHi) {
  std::size_t n = sa.values.size();
  if (n < 4 || sa.timestamps.size() != n) {
    return 0.0;
  }
  if (n % 2 != 0) {
    --n;  // kiss_fftr needs an even length
  }
  const double dt_seconds =
      static_cast<double>(sa.timestamps[n - 1] - sa.timestamps[0]) / (static_cast<double>(n - 1) * 1e9);
  if (dt_seconds <= 0.0) {
    return 0.0;
  }
  double mean = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    mean += sa.values[i];
  }
  mean /= static_cast<double>(n);

  std::vector<kiss_fft_scalar> input(n);
  for (std::size_t i = 0; i < n; ++i) {
    input[i] = static_cast<kiss_fft_scalar>(sa.values[i] - mean);
  }
  std::vector<kiss_fft_cpx> out(n / 2 + 1);
  kiss_fftr_cfg cfg = kiss_fftr_alloc(static_cast<int>(n), 0, nullptr, nullptr);
  if (cfg == nullptr) {
    return 0.0;
  }
  kiss_fftr(cfg, input.data(), out.data());
  KISS_FFT_FREE(cfg);

  const double nd = static_cast<double>(n);
  double power = 0.0;
  for (std::size_t i = 0; i < n / 2; ++i) {
    const double f = static_cast<double>(i) * (1.0 / dt_seconds) / nd;
    if (f >= fLo && f <= fHi) {
      const double amp = std::hypot(static_cast<double>(out[i].r), static_cast<double>(out[i].i)) / nd;
      power += amp * amp;
    }
  }
  return power;
}

}  // namespace

// ---------------------------------------------------------------------------
// runAnomalyScript — the shared Lua engine
// ---------------------------------------------------------------------------

std::vector<PJ::sdk::PlotMarker> runAnomalyScript(
    const std::string& code, const SeriesProvider& provider, std::string* error) {
  std::vector<PJ::sdk::PlotMarker> emitted;
  if (error != nullptr) {
    error->clear();
  }
  if (code.empty()) {
    if (error != nullptr) {
      *error = "empty rule";
    }
    return emitted;
  }

  sol::state lua;
  lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table);

  lua["series"] = [&provider, &lua](const std::string& name) -> sol::object {
    const SeriesAccessor* sa = provider.get ? provider.get(name) : nullptr;
    if (sa == nullptr) {
      return sol::make_object(lua, sol::lua_nil);
    }
    // Hand Lua a non-owning pointer to the accessor (the provider owns it and it
    // outlives the script run), so repeated series() calls don't copy the vectors.
    return sol::make_object(lua, sa);
  };
  lua["GetSeriesNames"] = [&provider]() -> std::vector<std::string> { return provider.names; };

  auto sa_type = lua.new_usertype<SeriesAccessor>("_SeriesAccessor");
  sa_type["size"] = &SeriesAccessor::size;
  sa_type["at"] = [](const SeriesAccessor& sa, std::size_t index, sol::this_state s) -> sol::object {
    auto pt = sa.at(index);
    if (!pt) {
      return sol::make_object(s, sol::lua_nil);
    }
    sol::state_view lv(s);
    sol::table r = lv.create_table();
    r["t"] = pt->t;
    r["v"] = pt->v;
    return r;
  };
  sa_type["atTime"] = &SeriesAccessor::atTime;

  // Spectral helper: summed FFT power in [fLo, fHi] Hz (DC-removed) — vibration.
  lua["bandPower"] = [](const SeriesAccessor& sa, double fLo, double fHi) -> double {
    return computeBandPower(sa, fLo, fHi);
  };

  std::optional<std::int64_t> open_region;

  // Time region: startMarker(t) opens, closeMarker(t, opts?) commits.
  lua["startMarker"] = [&open_region](double t) { open_region = static_cast<std::int64_t>(t); };
  lua["closeMarker"] = [&emitted, &open_region](double t, sol::optional<sol::table> opts) {
    if (!open_region) {
      return;
    }
    const auto a = *open_region;
    const auto b = static_cast<std::int64_t>(t);
    PJ::sdk::PlotMarker m;
    m.kind = PJ::sdk::MarkerKind::kRegion;
    m.t_start = std::min(a, b);
    m.t_end = std::max(a, b);
    m.severity = PJ::sdk::MarkerSeverity::kWarning;
    m.category = "anomaly";
    applyOpts(m, opts);  // label is opt-in (only drawn as a pill when opts.label is set)
    emitted.push_back(std::move(m));
    open_region.reset();
  };

  // Unified event: only x -> vertical line; only y -> horizontal line; both -> point.
  lua["createEvent"] = [&emitted](sol::optional<double> x, sol::optional<double> y, sol::optional<sol::table> opts) {
    PJ::sdk::PlotMarker m;
    m.category = "anomaly";
    if (x && y) {
      m.kind = PJ::sdk::MarkerKind::kEvent;  // point at (t, value)
      m.t_start = static_cast<std::int64_t>(*x);
      m.value_low = *y;
      m.has_value = true;
      m.severity = PJ::sdk::MarkerSeverity::kError;
    } else if (x) {
      m.kind = PJ::sdk::MarkerKind::kEvent;  // vertical line at time x (no value)
      m.t_start = static_cast<std::int64_t>(*x);
      m.severity = PJ::sdk::MarkerSeverity::kError;
    } else if (y) {
      m.kind = PJ::sdk::MarkerKind::kValueBand;  // horizontal line at value y (zero-height band)
      m.value_low = *y;
      m.value_high = *y;
      m.severity = PJ::sdk::MarkerSeverity::kWarning;
    } else {
      return;  // neither x nor y -> nothing to draw
    }
    applyOpts(m, opts);
    emitted.push_back(std::move(m));
  };

  // Value band: a shaded horizontal region [low, high].
  lua["createDataEvent"] = [&emitted](double low, double high, sol::optional<sol::table> opts) {
    PJ::sdk::PlotMarker m;
    m.kind = PJ::sdk::MarkerKind::kValueBand;
    m.value_low = low;
    m.value_high = high;
    m.severity = PJ::sdk::MarkerSeverity::kInfo;
    m.category = "anomaly";
    applyOpts(m, opts);
    emitted.push_back(std::move(m));
  };

  auto result = lua.safe_script(code, sol::script_pass_on_error);
  if (!result.valid()) {
    const sol::error err = result;
    if (error != nullptr) {
      *error = err.what();
    }
    emitted.clear();
    return emitted;
  }
  return emitted;
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
  rule.fail_on = parseSeverity(r.value("fail_on", std::string("error")));
  if (rule.code.empty()) {
    if (error != nullptr) {
      *error = "rule has no 'code'";
    }
    return std::nullopt;
  }
  return rule;
}

}  // namespace anomaly_core
