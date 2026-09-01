// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
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
// shape MCP and every OpenAI-style function-spec consumer both want), so a
// single registry drives the MCP `tools/list` response, any function-spec
// serialization, AND the unit tests — no second source of truth to drift.
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
  [[nodiscard]] nlohmann::json toFunctionSpecs() const;  // OpenAI-style function specs
  [[nodiscard]] nlohmann::json toMcpToolsList() const;   // MCP tools/list entries

 private:
  void add(ToolSpec spec);
  std::vector<ToolSpec> tools_;
};

// A plain-text listing of what is currently loaded — topics, their fields and
// types — for handing to the model up front so it does not have to ask.
//
// This is metadata the host already holds, so building it is free; it
// deliberately carries no statistics (min/max would mean scanning every series,
// which just moves the cost rather than removing it).
//
// Bounded by `budget_chars`, degrading in two steps as the dataset grows: the
// full tree, then topic names only. Whenever anything is left out the text says
// so explicitly — a model that believes an incomplete listing is the whole
// truth will confidently tell the user a signal does not exist.
[[nodiscard]] std::string catalogDigest(const PJ::sdk::ToolboxHostView& host, std::size_t budget_chars = 6000);

// A resolved "topic/field" curve path: the field handle plus the owning topic
// name. `path` is the canonical form the lookup settled on — with several
// datasets loaded it carries the "dataset:topic/field" qualifier, so whatever
// echoes it back also discloses which dataset it came from. `host_path` is the
// bare topic/field form: the HOST's create interface addresses inputs by name
// and knows nothing of the qualifier, so anything handed to it goes in this
// form (reads are unaffected — they go by handle).
struct ResolvedSeries {
  PJ::sdk::FieldHandle handle;
  std::string topic;
  std::string path;
  std::string host_path;
};

// Outcome of a path lookup. When nothing resolves, `candidates` carries the
// near misses so the caller can put them in the error — a model that gets told
// what the real paths are corrects on the spot, instead of spending a whole
// extra round-trip asking the catalog.
struct SeriesLookup {
  std::optional<ResolvedSeries> resolved;
  std::vector<std::string> candidates;
  bool ambiguous = false;  // several paths matched; refusing to guess between them
};

// Resolve one curve path against the catalog. Accepts the host's
// "dataset:topic/field" qualifier (matched against known source names); an
// unqualified path whose exact topic/field exists in several datasets is
// refused as ambiguous with the qualified candidates. Exported for the unit
// tests: resolution is where the multi-dataset rules live.
[[nodiscard]] SeriesLookup resolveSeriesPath(const PJ::sdk::CatalogSnapshot& catalog, const std::string& series);

}  // namespace assistant_agent
