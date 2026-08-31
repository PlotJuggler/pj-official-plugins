// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Live smoke for the Claude Code backend: spawns the real `claude` CLI wired to
// the in-plugin MCP server and checks it can call a tool and answer. Opt-in —
// runs only with ASSISTANT_CLAUDE_SMOKE=1 (needs a logged-in Claude CLI; it
// consumes the user's subscription). Skipped otherwise, so CI stays offline.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>
#include <vector>

#include "claude_backend.hpp"
#include "tool_registry.hpp"

namespace {

using assistant_agent::BackendEvent;
using assistant_agent::ClaudeBackend;
using assistant_agent::ToolContext;
using assistant_agent::ToolRegistry;
using assistant_agent::TurnTools;

TEST(ClaudeSmoke, ListTopicsThroughMcp) {
  if (std::getenv("ASSISTANT_CLAUDE_SMOKE") == nullptr) {
    GTEST_SKIP() << "set ASSISTANT_CLAUDE_SMOKE=1 (needs a logged-in `claude` CLI) to run";
  }

  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/imu/accel");
  store.addField("/imu/accel", "x", {0}, {1.0});
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(store.makeHost());

  TurnTools tools;
  tools.registry = &reg;
  // Direct execution (no GuiExecutor): the MCP server thread runs the tool.
  tools.invoke = [&](const std::string& n, const nlohmann::json& a) { return reg.execute(n, a, ctx); };

  const char* cli = std::getenv("ASSISTANT_CLAUDE_CLI");
  ClaudeBackend backend(cli != nullptr ? cli : "claude", "");

  std::vector<BackendEvent> events;
  std::mutex mu;
  backend.sendUserMessage(
      "Call the list_topics tool and tell me the exact topic name it returns.", tools, [&](BackendEvent e) {
        std::lock_guard<std::mutex> lk(mu);
        std::cerr << "[event " << static_cast<int>(e.kind) << "] " << e.text << "\n";
        events.push_back(std::move(e));
      });

  bool complete = false;
  bool mentioned_topic = false;
  bool called_tool = false;
  for (const auto& e : events) {
    if (e.kind == BackendEvent::Kind::TurnComplete) {
      complete = true;
    }
    if (e.kind == BackendEvent::Kind::ToolActivity && e.text.find("list_topics") != std::string::npos) {
      called_tool = true;
    }
    if (e.text.find("/imu/accel") != std::string::npos) {
      mentioned_topic = true;
    }
  }
  EXPECT_TRUE(complete) << "backend never signalled TurnComplete";
  EXPECT_TRUE(called_tool) << "Claude did not call list_topics via MCP";
  EXPECT_TRUE(mentioned_topic) << "answer did not mention the topic the tool returned";
}

}  // namespace

// The one thing about this backend that is not a preference.
//
// Headless Claude reaches the user's machine through the CLI's built-in tools —
// Bash, Read, Write, the rest — and `--tools ""` is what withholds every one of
// them. Take that flag away and this plugin's assistant can read any file the
// user can. The guarantee in ROADMAP.md ("structurally incapable of destroying
// anything") rests on three entries in a vector that nothing was watching.
//
// The pressure to relax it is real and will come with a good reason attached:
// letting the model actually LOOK at an exported image needs `Read`, and that is
// a genuinely useful feature. It still does not go here. A bounded MCP tool of
// ours can hand over one image; `Read` hands over the filesystem.
TEST(ClaudeBackendCommandLine, WithholdsEveryBuiltInTool) {
  ToolRegistry registry;
  const std::vector<std::string> argv = assistant_agent::buildClaudeArgv(
      "/usr/bin/claude", "/tmp/mcp.json", assistant_agent::allowedToolsArg(registry), "system", "sonnet", "");

  const auto at = std::find(argv.begin(), argv.end(), "--tools");
  ASSERT_NE(at, argv.end()) << "--tools is gone: every built-in tool is now available to the model";
  ASSERT_NE(std::next(at), argv.end()) << "--tools has no value";
  EXPECT_EQ(*std::next(at), "") << "--tools must be empty; anything else grants a built-in tool";

  EXPECT_NE(std::find(argv.begin(), argv.end(), "--strict-mcp-config"), argv.end())
      << "without it the CLI may load MCP servers from the user's own config";

  // The isolation half of the lock: --restricted makes the CLI ignore the
  // machine's user/project/local settings AND the user-level CLAUDE.md, so the
  // panel's register and instructions do not depend on who installed it or on
  // what they configured for their own coding sessions. (The project-level side
  // is covered by running the CLI in a private cwd, which argv cannot show.)
  EXPECT_NE(std::find(argv.begin(), argv.end(), "--restricted"), argv.end())
      << "without it the panel inherits the machine's settings, output style and global CLAUDE.md";
}

// Whitelisting is the other half: --tools decides what EXISTS, --allowedTools
// what may be called. Every entry has to be one of ours, so a built-in cannot be
// let back in through this door.
TEST(ClaudeBackendCommandLine, AllowsOnlyThisPluginsOwnTools) {
  ToolRegistry registry;
  const std::string allowed = assistant_agent::allowedToolsArg(registry);
  ASSERT_FALSE(allowed.empty()) << "the registry advertised no tools at all";

  std::size_t start = 0;
  int counted = 0;
  while (start <= allowed.size()) {
    const std::size_t comma = allowed.find(',', start);
    const std::string entry = allowed.substr(start, comma - start);
    EXPECT_EQ(entry.rfind("mcp__pj__", 0), 0u)
        << "'" << entry << "' is not one of this plugin's MCP tools; a built-in must never be whitelisted";
    ++counted;
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  EXPECT_EQ(counted, static_cast<int>(registry.tools().size())) << "the whitelist and the registry disagree";
}
