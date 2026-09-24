// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace assistant_agent {

// Wrap a per-sample Luau body into a self-describing filter class the host runs
// as an eager DerivedEngine node (via DataProcessorsHostView::createTransform).
// Ported from toolbox_transform_editor's buildTransformScript (Luau path only).
//
// The user body runs with `time`, `value`, and `v1..v<num_extra>` in scope
// (value = the primary input; v1..vN = the additional inputs, in order). The
// global section runs once per instance inside the factory closure, so its
// locals persist across calls (PJ3 global-variable semantics). Output count is
// decided host-side by the `outputs` passed to createTransform; `:calculate`
// forwards the body's results unchanged (MULTRET).
// Escape a model-supplied string for embedding inside a double-quoted Lua
// string literal (names, labels and series paths land verbatim in generated
// scripts).
inline std::string luaStringEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

// Wrap a model-supplied body into an on-demand Luau chunk (kind="on_demand",
// the data-processors surface a consumer-requested-time script uses instead
// of a per-sample transform). `inputs` (a table keyed by each declared
// input's LITERAL name — the exact string this node's request carries, per
// the host's on_demand contract: "the script reads each input under its
// literal name") and `params` (this node's params_json, decoded) are bound
// at the chunk's own top via Lua's vararg `...`; the body then reads
// inputs["<topic>"] and returns a table of the declared outputs, by name.
// Evaluated once per requested instant/window sample, not per data sample,
// so unlike buildLuauTransform there is no factory/calculate split — the
// body IS the chunk.
inline std::string buildOnDemandChunk(const std::string& body) {
  std::string src = "-- pj-script: luau\n";
  src += "local inputs, params = ...\n";
  src += body + "\n";
  return src;
}

inline std::string buildLuauTransform(
    const std::string& id, const std::string& name, const std::string& global_code, const std::string& body,
    std::size_t num_extra) {
  std::string params = "time, value";
  for (std::size_t k = 0; k < num_extra; ++k) {
    params += ", v" + std::to_string(k + 1);
  }

  std::string src = "-- pj-script: luau\n";
  src += "local function _pj_make()\n";
  src += global_code + "\n";
  src += "  return function(" + params + ")\n";
  src += body + "\n";
  src += "  end\n";
  src += "end\n";
  src += "local T = { id = \"" + luaStringEscape(id) + "\", name = \"" + luaStringEscape(name) +
         "\", output = \"double\" }\n";
  src += "T.__index = T\n";
  src += "function T.create(_) return setmetatable({ fn = _pj_make() }, T) end\n";
  src += "function T:calculate(t, v, ...) return self.fn(t, v, ...) end\n";
  src += "return T\n";
  return src;
}

}  // namespace assistant_agent
