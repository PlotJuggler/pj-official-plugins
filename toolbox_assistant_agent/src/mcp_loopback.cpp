// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "mcp_loopback.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>  // unlink for the private MCP config file
#endif

#include "private_file.hpp"  // writePrivateTempFile

namespace assistant_agent {

McpLoopback::~McpLoopback() {
  if (!config_path_.empty()) {
    unlink(config_path_.c_str());
  }
}

bool McpLoopback::ensure(const TurnTools& tools, std::string& err) {
  if (server_) {
    return true;
  }
  if (tools.registry == nullptr || !tools.invoke) {
    err = "no tool registry/invoker for this turn";
    return false;
  }
  server_ = std::make_unique<McpHttpServer>(*tools.registry, tools.invoke);
  if (!server_->start()) {
    server_.reset();
    err = "could not bind a local MCP port";
    return false;
  }
  return true;
}

std::string McpLoopback::url() const {
  return server_ ? server_->url() : std::string{};
}

std::string McpLoopback::token() const {
  return server_ ? server_->token() : std::string{};
}

std::string McpLoopback::configFilePath() {
  if (!config_path_.empty()) {
    return config_path_;
  }
  if (!server_) {
    return {};
  }
  // The config carries the per-session bearer token; hand it to the CLI as a
  // private file rather than a command-line argument (argv is world-readable
  // via /proc/<pid>/cmdline). mkstemp creates it 0600.
  if (!writePrivateTempFile("/tmp/pj_assistant_mcp_", server_->mcpConfigJson(), config_path_)) {
    return {};
  }
  return config_path_;
}

}  // namespace assistant_agent
