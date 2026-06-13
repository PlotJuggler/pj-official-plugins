#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// CustomFunctionEngine — Lua evaluation core for the Custom Function toolbox.
//
// Faithful port of PlotJuggler 3's transforms/lua_custom_function.cpp. A custom
// function derives ONE output series from a "main" input series plus N optional
// "additional" sources. The user-supplied body becomes the body of a Lua
// function with the exact PJ3 signature:
//
//     function calc(time, value, v1, v2, ... vN)
//       <user body>
//     end
//
// where, for each sample i of the main series:
//   - `time`  = main timestamp[i]
//   - `value` = main value[i]
//   - `vk`    = additional[k] value sampled at `time` (nearest sample; NaN if
//               the additional series is empty)
//
// The body returns one of (matching PJ3 LuaCustomFunction::calculatePoints):
//   - a single number          -> point (time, number)
//   - two numbers (t, v)       -> point (t, v)
//   - a table of {t, v} pairs  -> several points
//
// Header-only (sol2 in the header) so the engine is unit-testable without the
// plugin host. No Qt, no datastore — input/output are plain doubles.

#include <array>
#include <cmath>
#include <optional>
#include <sol/sol.hpp>
#include <string>
#include <vector>

namespace pj_custom_function {

/// Read-only timestamp/value view over one input series (timestamps ascending).
struct SeriesAccessor {
  std::vector<double> timestamps;
  std::vector<double> values;

  [[nodiscard]] size_t size() const {
    return timestamps.size();
  }
  [[nodiscard]] bool empty() const {
    return timestamps.empty();
  }

  /// Value at the sample whose timestamp is closest to `t` (PJ3 getIndexFromX
  /// semantics). Returns NaN when the series is empty.
  [[nodiscard]] double valueAtNearest(double t) const {
    if (timestamps.empty()) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    auto it = std::lower_bound(timestamps.begin(), timestamps.end(), t);
    if (it == timestamps.end()) {
      return values.back();
    }
    if (it == timestamps.begin()) {
      return values.front();
    }
    size_t idx = static_cast<size_t>(it - timestamps.begin());
    // Pick whichever of idx-1 / idx is closer in time.
    double d_hi = timestamps[idx] - t;
    double d_lo = t - timestamps[idx - 1];
    return (d_lo <= d_hi) ? values[idx - 1] : values[idx];
  }
};

/// One derived output sample.
struct OutputPoint {
  double t;
  double v;
};

/// Compiles + runs a PJ3-style custom function. Construct, compile() once, then
/// evaluate() as the main series grows (incremental via `after_timestamp`).
class CustomFunctionEngine {
 public:
  /// Compile `global_code` (run once, may define helpers/vars) and wrap
  /// `function_body` into `calc(time, value, v1..vN)` for `num_additional`
  /// extra sources. Returns "" on success or a human-readable error.
  std::string compile(const std::string& global_code, const std::string& function_body, size_t num_additional) {
    num_additional_ = num_additional;
    lua_ = sol::state{};
    lua_.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table);
    calc_ = sol::protected_function{};

    if (!global_code.empty()) {
      auto result = lua_.safe_script(global_code, sol::script_pass_on_error);
      if (!result.valid()) {
        sol::error err = result;
        return std::string("Global: ") + err.what();
      }
    }

    std::string signature = "function calc(time, value";
    for (size_t i = 1; i <= num_additional; ++i) {
      signature += ", v" + std::to_string(i);
    }
    signature += ")\n" + function_body + "\nend";

    auto result = lua_.safe_script(signature, sol::script_pass_on_error);
    if (!result.valid()) {
      sol::error err = result;
      return err.what();
    }
    calc_ = lua_.get<sol::protected_function>("calc");
    if (!calc_.valid()) {
      return "internal error: calc() not defined";
    }
    return "";
  }

  /// Evaluate over every main sample with `timestamp > after_timestamp`,
  /// looking up each additional source at the main timestamp. Appends derived
  /// points to `out`. Returns "" on success or a human-readable error.
  std::string evaluate(
      const SeriesAccessor& main, const std::vector<const SeriesAccessor*>& additional, double after_timestamp,
      std::vector<OutputPoint>& out) {
    if (!calc_.valid()) {
      return "internal error: function not compiled";
    }
    if (additional.size() != num_additional_) {
      return "internal error: additional source count mismatch";
    }

    std::vector<double> args;
    args.reserve(num_additional_ + 2);

    for (size_t i = 0; i < main.size(); ++i) {
      const double t = main.timestamps[i];
      if (t <= after_timestamp) {
        continue;
      }
      args.clear();
      args.push_back(t);
      args.push_back(main.values[i]);
      for (const SeriesAccessor* src : additional) {
        args.push_back(src != nullptr ? src->valueAtNearest(t) : std::numeric_limits<double>::quiet_NaN());
      }

      sol::protected_function_result result = calc_(sol::as_args(args));
      if (!result.valid()) {
        sol::error err = result;
        return err.what();
      }
      if (std::string e = appendResult(result, t, out); !e.empty()) {
        return e;
      }
    }
    return "";
  }

 private:
  // Interpret the Lua return value(s) exactly like PJ3 LuaCustomFunction.
  static std::string appendResult(sol::protected_function_result& result, double time, std::vector<OutputPoint>& out) {
    const int count = result.return_count();
    if (count >= 2 && result.get_type(0) == sol::type::number && result.get_type(1) == sol::type::number) {
      out.push_back({result.get<double>(0), result.get<double>(1)});
      return "";
    }
    if (count == 1 && result.get_type(0) == sol::type::number) {
      out.push_back({time, result.get<double>(0)});
      return "";
    }
    if (count == 1 && result.get_type(0) == sol::type::table) {
      sol::table table = result.get<sol::table>(0);
      for (size_t i = 1; i <= table.size(); ++i) {
        sol::object element = table.get<sol::object>(i);
        if (!element.is<sol::table>()) {
          return "Wrong return object: expected an array of {time, value} pairs";
        }
        sol::table pair = element.as<sol::table>();
        out.push_back({pair[1].get<double>(), pair[2].get<double>()});
      }
      return "";
    }
    return "Wrong return object: expecting either a single value, two values (time, value) "
           "or an array of two-sized arrays (time, value)";
  }

  sol::state lua_;
  sol::protected_function calc_;
  size_t num_additional_ = 0;
};

}  // namespace pj_custom_function
