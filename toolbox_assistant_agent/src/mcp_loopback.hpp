// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <string>

#include "llm_backend.hpp"  // TurnTools
#include "mcp_http_server.hpp"

namespace assistant_agent {

// The loopback MCP server, in the form every harness backend needs it: bring
// it up lazily on the first turn, bound to that turn's tool surface, and hand
// out its url()/token() to whichever harness is driving it. Extracted out of
// ClaudeBackend so CodexBackend can share it — Claude needs the bearer token
// wrapped in a `--mcp-config` file (configFilePath()); Codex takes the url and
// token directly as `-c` flags / an env var, so it never calls
// configFilePath() at all.
class McpLoopback {
 public:
  ~McpLoopback();

  // Bring the server up on first call, bound to `tools`; a no-op success on
  // every later call in this backend's lifetime. False (with `err` set) if no
  // loopback port was free, or this turn carries no tool registry/invoker.
  bool ensure(const TurnTools& tools, std::string& err);

  [[nodiscard]] std::string url() const;
  [[nodiscard]] std::string token() const;

  // Lazily writes the Claude-style `--mcp-config` JSON (one "pj" http server,
  // the bearer token in an Authorization header) to a `mkstemp` 0600 file and
  // returns its path, memoized for the life of this object; the destructor
  // unlinks it. Requires ensure() to have already brought the server up —
  // returns "" (no error detail; the caller has none to give beyond "could not
  // write the file") if it has not, or if the file could not be created/written.
  [[nodiscard]] std::string configFilePath();

 private:
  std::unique_ptr<McpHttpServer> server_;
  // 0600 temp file holding the MCP config (bearer token inside); created on
  // the first configFilePath() call, removed in the destructor.
  std::string config_path_;
};

}  // namespace assistant_agent
