// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdlib>
#include <pj_base/number_parse.hpp>
#include <pj_query/evaluator.hpp>
#include <string>
#include <string_view>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace mosaico {

using PJ::query::Query;
using PJ::query::ValidationResult;

// Lua-based metadata query engine; the plugin-side PJ::query::Evaluator.
//
// Usage:
//   Engine engine;
//   engine.set(metadata);
//   auto ok = engine.eval(query);   // Query object or string_view
//
// Metadata keys are injected as Lua globals. The query is expanded
// (shorthand resolved) by the Query class before Lua sees it.
class Engine : public PJ::query::Evaluator {
 public:
  Engine() {
    lua_.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table);
  }

  void set(const PJ::query::Metadata& metadata) {
    for (const auto& [key, value] : metadata) {
      if (auto num = PJ::parseNumber<double>(value)) {
        lua_[key] = *num;
      } else {
        lua_[key] = value;
      }
    }
  }

  void clear(const PJ::query::Metadata& metadata) {
    for (const auto& [key, value] : metadata) {
      lua_[key] = sol::lua_nil;
    }
  }

  // Evaluate a pre-parsed Query. Uses the expanded Lua string.
  [[nodiscard]] bool eval(const Query& query) const noexcept {
    try {
      const auto& lua_str = query.expanded();
      if (lua_str.empty()) {
        return false;
      }

      auto wrapped = std::string("return (") + lua_str + ")";
      auto result = lua_.safe_script(wrapped, sol::script_pass_on_error);
      if (!result.valid()) {
        return false;
      }

      sol::object val = result;
      return truthy(val);
    } catch (...) {
      return false;
    }
  }

  // Convenience: evaluate a raw string. Query::expanded() resolves shorthand
  // and preserves raw input (function calls, nil comparisons) verbatim.
  [[nodiscard]] bool eval(std::string_view query_str) const noexcept {
    try {
      if (query_str.empty()) {
        return false;
      }
      return eval(Query(query_str));
    } catch (...) {
      return false;
    }
  }

  // PJ::query::Evaluator seam for the shared visible-row filter.
  [[nodiscard]] ValidationResult validate(std::string_view text) override {
    return validateText(text);
  }
  [[nodiscard]] bool evaluate(const Query& query, const PJ::query::Metadata& metadata) override {
    set(metadata);
    const bool match = eval(query);
    clear(metadata);
    return match;
  }

  // Validate using Lua's parser (definitive syntax check).
  [[nodiscard]] static ValidationResult validateText(std::string_view query_str) noexcept {
    try {
      if (query_str.empty()) {
        ValidationResult r;
        r.error = "empty query";
        return r;
      }

      // Expand shorthand via Query, then check with Lua.
      auto lua_str = Query(query_str).expanded();
      if (lua_str.empty()) {
        lua_str = std::string(query_str);
      }

      // Reuse one lua_State per thread. `load()` only parses bytecode and
      // doesn't mutate globals, so a failed load leaves the state clean for
      // the next call. Fresh-constructing sol::state here ran a full
      // luaL_newstate + teardown on every keystroke (QueryBar::onTextChanged
      // hits this on each edit), wasting ~10 heap cycles/sec while typing.
      thread_local sol::state tmp;
      auto wrapped = std::string("return (") + lua_str + ")";
      auto result = tmp.load(wrapped);
      if (!result.valid()) {
        sol::error err = result;
        return parse_error(err.what());
      }
      return ValidationResult{true, {}, 0, 0};
    } catch (const std::exception& e) {
      ValidationResult r;
      r.error = e.what();
      return r;
    } catch (...) {
      ValidationResult r;
      r.error = "unknown error";
      return r;
    }
  }

 private:
  [[nodiscard]] static bool truthy(const sol::object& obj) {
    auto t = obj.get_type();
    if (t == sol::type::boolean) {
      return obj.as<bool>();
    }
    return t != sol::type::lua_nil && t != sol::type::none;
  }

  [[nodiscard]] static ValidationResult parse_error(const std::string& msg) {
    ValidationResult r;
    r.valid = false;
    r.error = msg;

    auto colon1 = msg.find(':');
    if (colon1 == std::string::npos) {
      return r;
    }
    auto colon2 = msg.find(':', colon1 + 1);
    if (colon2 == std::string::npos) {
      return r;
    }

    auto line_str = msg.substr(colon1 + 1, colon2 - colon1 - 1);
    char* end = nullptr;
    long line = std::strtol(line_str.data(), &end, 10);
    if (end != line_str.data()) {
      r.line = static_cast<int>(line);
    }
    return r;
  }

  mutable sol::state lua_;
};

}  // namespace mosaico
