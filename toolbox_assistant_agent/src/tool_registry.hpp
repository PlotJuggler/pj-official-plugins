// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "tool_context.hpp"

namespace assistant_agent {

// Runs one tool call and returns its result, blocking the caller's thread until
// the GUI thread executes it (see GuiExecutor). This is the seam a backend uses
// to run a tool the model asked for, without ever touching host services from
// its own worker thread.
using ToolInvoker = std::function<ToolResult(const std::string& name, const nlohmann::json& args)>;

// One tool the assistant can call. `input_schema` is a JSON Schema object (the
// same shape Ollama and MCP both want), so a single registry drives the Ollama
// `tools` array, the MCP `tools/list` response, AND the unit tests — no second
// source of truth to drift.
struct ToolSpec {
  std::string name;
  std::string description;
  nlohmann::json input_schema;
  std::function<ToolResult(const nlohmann::json& args, ToolContext& ctx)> executor;
};

// The fixed catalog of assistant tools. Read-only + additive by construction:
// list/describe/read topics, create derived series + markers, report status.
// There is deliberately no delete/destroy tool — the plugin ABI exposes no such
// host op, so one could not be built even if asked for.
class ToolRegistry {
 public:
  ToolRegistry();

  [[nodiscard]] const std::vector<ToolSpec>& tools() const {
    return tools_;
  }
  [[nodiscard]] const ToolSpec* find(std::string_view name) const;

  // Execute by name. Unknown tool -> a failure ToolResult (never throws), so a
  // model that hallucinates a tool name gets a correctable error, not a crash.
  [[nodiscard]] ToolResult execute(std::string_view name, const nlohmann::json& args, ToolContext& ctx) const;

  // Serialize the catalog for each backend's tool-advertisement format.
  [[nodiscard]] nlohmann::json toOllamaTools() const;   // OpenAI-style function specs
  [[nodiscard]] nlohmann::json toMcpToolsList() const;  // MCP tools/list entries

 private:
  void add(ToolSpec spec);
  std::vector<ToolSpec> tools_;
};

}  // namespace assistant_agent
