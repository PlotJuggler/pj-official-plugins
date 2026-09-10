// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Codex backend: the command-line lock, the fake-CLI resume/translation
// tests, the parser (codex_stream.hpp), and the live opt-in smoke — mirrors
// claude_smoke_test.cpp's structure for the Codex harness.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include <optional>
#include <pj_base/sdk/platform.hpp>

#include "codex_backend.hpp"
#include "codex_stream.hpp"
#include "support/backend_test_helpers.hpp"
#include "tool_registry.hpp"

namespace {

using assistant_agent::BackendEvent;
using assistant_agent::CodexBackend;
using assistant_agent::CodexEvent;
using assistant_agent::HarnessMemory;
using assistant_agent::parseCodexLine;
using assistant_agent::ToolRegistry;
using assistant_agent::TurnTools;
using assistant_agent::testing::runListTopicsSmoke;

// The one thing about this backend that is not a preference.
//
// Codex has no `--tools ""` equivalent: every call, ours included, runs
// inside Code Mode (a JavaScript host with no `require`, `process` or
// `fetch`), so disabling it would also disable our own MCP tools -- it stays
// on by construction. What withholds the rest of the machine is the four `-c`
// lines (shell_tool, unified_exec, web_search, view_image) plus the seven
// `--disable`s, `sandbox_mode="read-only"` and `approval_policy="never"`.
// Take any of them away and this plugin's Codex-backed assistant gains a
// built-in capability nothing here asked for.
TEST(CodexBackendCommandLine, WithholdsEveryBuiltInTool) {
  const std::vector<std::string> argv = assistant_agent::buildCodexArgv(
      "/usr/bin/codex", "/tmp/work", "http://127.0.0.1:1234/mcp", "PJ_ASSISTANT_MCP_TOKEN", "/tmp/instructions.txt",
      "gpt-5-codex", "");

  auto has = [&](const std::string& entry) { return std::find(argv.begin(), argv.end(), entry) != argv.end(); };

  EXPECT_TRUE(has("--json"));
  EXPECT_TRUE(has("--skip-git-repo-check"));
  EXPECT_TRUE(has("--ignore-user-config"));
  EXPECT_TRUE(has("--ignore-rules"));

  EXPECT_TRUE(has("features.shell_tool=false"));
  EXPECT_TRUE(has("features.unified_exec=false"));
  EXPECT_TRUE(has(R"(web_search="disabled")"));
  EXPECT_TRUE(has("tools.view_image=false"));
  EXPECT_TRUE(has(R"(sandbox_mode="read-only")"));
  EXPECT_TRUE(has(R"(approval_policy="never")"));

  for (const char* disabled :
       {"view_image", "memories", "shell_snapshot", "multi_agent", "plugins", "apps", "skill_search"}) {
    EXPECT_TRUE(has(disabled)) << disabled << " is missing from the --disable list";
  }
  // Every --disable value above must actually follow a --disable flag, not
  // just appear somewhere in argv by coincidence.
  for (const char* disabled :
       {"view_image", "memories", "shell_snapshot", "multi_agent", "plugins", "apps", "skill_search"}) {
    const auto at = std::find(argv.begin(), argv.end(), std::string(disabled));
    ASSERT_NE(at, argv.begin()) << disabled;
    EXPECT_EQ(*std::prev(at), "--disable") << disabled << " does not follow --disable";
  }

  EXPECT_TRUE(has(R"(model_instructions_file="/tmp/instructions.txt")"));
  EXPECT_TRUE(has(R"(mcp_servers.pj.url="http://127.0.0.1:1234/mcp")"));
  EXPECT_TRUE(has(R"(mcp_servers.pj.bearer_token_env_var="PJ_ASSISTANT_MCP_TOKEN")"));
  EXPECT_TRUE(has("mcp_servers.pj.required=true"));
  EXPECT_TRUE(has(R"(mcp_servers.pj.default_tools_approval_mode="approve")"));

  ASSERT_FALSE(argv.empty());
  EXPECT_EQ(argv.back(), "-") << "the prompt must be read from stdin, marked by a trailing '-'";

  // The bearer token itself must never appear in argv (/proc/<pid>/cmdline is
  // world-readable) -- only the env VAR NAME does, via bearer_token_env_var.
  for (const auto& entry : argv) {
    EXPECT_EQ(entry.find("secret-token-value"), std::string::npos);
  }
}

TEST(CodexBackendCommandLine, DashCPresentOnlyWhenNotResuming) {
  const std::vector<std::string> fresh = assistant_agent::buildCodexArgv(
      "codex", "/tmp/work", "http://127.0.0.1:1234/mcp", "PJ_ASSISTANT_MCP_TOKEN", "/tmp/instructions.txt", "", "");
  const auto dash_c = std::find(fresh.begin(), fresh.end(), "-C");
  ASSERT_NE(dash_c, fresh.end());
  ASSERT_NE(std::next(dash_c), fresh.end());
  EXPECT_EQ(*std::next(dash_c), "/tmp/work");
  EXPECT_EQ(std::find(fresh.begin(), fresh.end(), "resume"), fresh.end());

  const std::vector<std::string> resumed = assistant_agent::buildCodexArgv(
      "codex", "/tmp/work", "http://127.0.0.1:1234/mcp", "PJ_ASSISTANT_MCP_TOKEN", "/tmp/instructions.txt", "",
      "thread-123");
  EXPECT_EQ(std::find(resumed.begin(), resumed.end(), "-C"), resumed.end())
      << "-C must be absent on resume: the process cwd is irrelevant to it";
  ASSERT_GE(resumed.size(), 4u);
  EXPECT_EQ(resumed[1], "exec");
  EXPECT_EQ(resumed[2], "resume") << "resume must follow `exec` immediately";
  EXPECT_EQ(resumed[3], "thread-123");
}

TEST(CodexBackendCommandLine, ModelFlagOmittedWhenEmpty) {
  const std::vector<std::string> argv = assistant_agent::buildCodexArgv(
      "codex", "/tmp/work", "http://127.0.0.1:1234/mcp", "PJ_ASSISTANT_MCP_TOKEN", "/tmp/instructions.txt", "", "");
  EXPECT_EQ(std::find(argv.begin(), argv.end(), "-m"), argv.end());

  const std::vector<std::string> with_model = assistant_agent::buildCodexArgv(
      "codex", "/tmp/work", "http://127.0.0.1:1234/mcp", "PJ_ASSISTANT_MCP_TOKEN", "/tmp/instructions.txt",
      "gpt-5-codex", "");
  const auto m = std::find(with_model.begin(), with_model.end(), "-m");
  ASSERT_NE(m, with_model.end());
  ASSERT_NE(std::next(m), with_model.end());
  EXPECT_EQ(*std::next(m), "gpt-5-codex");
}

// --- codex_stream.hpp: parseCodexLine, one event per shape -----------------

TEST(ParseCodexLine, ThreadStartedCapturesSessionId) {
  const CodexEvent ev = parseCodexLine(R"({"type":"thread.started","thread_id":"01a06602-abc"})");
  EXPECT_EQ(ev.kind, CodexEvent::Kind::ThreadStarted);
  EXPECT_EQ(ev.session_id, "01a06602-abc");
}

TEST(ParseCodexLine, TurnStarted) {
  EXPECT_EQ(parseCodexLine(R"({"type":"turn.started"})").kind, CodexEvent::Kind::TurnStarted);
}

TEST(ParseCodexLine, ToolCallStarted) {
  const CodexEvent ev = parseCodexLine(
      R"({"type":"item.started","item":{"id":"item_0","type":"mcp_tool_call","server":"pj","tool":"ping",)"
      R"("arguments":{"echo":"hello"},"result":null,"error":null,"status":"in_progress"}})");
  EXPECT_EQ(ev.kind, CodexEvent::Kind::ToolCallStarted);
  EXPECT_EQ(ev.tool_name, "ping");
}

TEST(ParseCodexLine, ToolCallCompletedSuccessfullyIsIgnored) {
  const CodexEvent ev = parseCodexLine(
      R"({"type":"item.completed","item":{"id":"item_0","type":"mcp_tool_call","server":"pj","tool":"ping",)"
      R"("arguments":{"echo":"hello"},"result":{"content":[{"type":"text","text":"pong: hello"}],)"
      R"("structured_content":null},"error":null,"status":"completed"}})");
  EXPECT_EQ(ev.kind, CodexEvent::Kind::Ignored);
}

TEST(ParseCodexLine, ToolCallFailedCapturesToolAndError) {
  const CodexEvent ev = parseCodexLine(
      R"({"type":"item.completed","item":{"id":"item_0","type":"mcp_tool_call","server":"pj","tool":"ping",)"
      R"("arguments":{"echo":"hello"},"result":null,)"
      R"("error":{"message":"MCP tool call requires approval, but approval policy is never"},"status":"failed"}})");
  ASSERT_EQ(ev.kind, CodexEvent::Kind::ToolCallFailed);
  EXPECT_EQ(ev.tool_name, "ping");
  EXPECT_EQ(ev.text, "MCP tool call requires approval, but approval policy is never");
}

TEST(ParseCodexLine, AgentMessageIsAssistantText) {
  const CodexEvent ev =
      parseCodexLine(R"({"type":"item.completed","item":{"id":"item_1","type":"agent_message","text":"pong"}})");
  EXPECT_EQ(ev.kind, CodexEvent::Kind::AssistantText);
  EXPECT_EQ(ev.text, "pong");
}

TEST(ParseCodexLine, StartupNoticeItemIsAnErrorItemRegardlessOfPosition) {
  // The parser does not know about turn order -- that's CodexBackend's job
  // (`in_turn`). It just reports what the record says.
  const CodexEvent ev = parseCodexLine(
      R"({"type":"item.completed","item":{"id":"item_0","type":"error",)"
      R"("message":"Code Mode is unavailable because code-mode host is disabled."}})");
  EXPECT_EQ(ev.kind, CodexEvent::Kind::ErrorItem);
  EXPECT_EQ(ev.text, "Code Mode is unavailable because code-mode host is disabled.");
}

TEST(ParseCodexLine, TurnCompletedFillsMetrics) {
  const CodexEvent ev = parseCodexLine(
      R"({"type":"turn.completed","usage":{"input_tokens":10275,"cached_input_tokens":100,)"
      R"("cache_write_input_tokens":50,"output_tokens":5,"reasoning_output_tokens":0}})");
  ASSERT_EQ(ev.kind, CodexEvent::Kind::TurnCompleted);
  EXPECT_TRUE(ev.metrics.valid);
  EXPECT_DOUBLE_EQ(ev.metrics.cost_usd, 0.0);
  EXPECT_EQ(ev.metrics.input_tokens, 10275);
  EXPECT_EQ(ev.metrics.output_tokens, 5);
  EXPECT_EQ(ev.metrics.cache_read_tokens, 100);
  EXPECT_EQ(ev.metrics.cache_creation_tokens, 50);
  EXPECT_EQ(ev.metrics.api_ms, 0);
}

TEST(ParseCodexLine, TurnFailedAndTopLevelErrorAreBothErrorlike) {
  const CodexEvent failed = parseCodexLine(R"({"type":"turn.failed","error":{"message":"boom"}})");
  EXPECT_EQ(failed.kind, CodexEvent::Kind::TurnFailed);
  EXPECT_EQ(failed.text, "boom");

  const CodexEvent top = parseCodexLine(R"({"type":"error","message":"kaboom"})");
  EXPECT_EQ(top.kind, CodexEvent::Kind::ErrorItem);
  EXPECT_EQ(top.text, "kaboom");
}

TEST(ParseCodexLine, MalformedJsonIsIgnoredNeverThrows) {
  EXPECT_EQ(parseCodexLine("not json at all").kind, CodexEvent::Kind::Ignored);
  EXPECT_EQ(parseCodexLine("").kind, CodexEvent::Kind::Ignored);
  EXPECT_EQ(parseCodexLine(R"({"type":"something.unheard.of"})").kind, CodexEvent::Kind::Ignored);
}

// --- CodexBackend, against a fake CLI (never spawns the real one) ----------

#if defined(__unix__) || defined(__APPLE__)
namespace {

using assistant_agent::testing::firstError;
using assistant_agent::testing::writeFakeCliScript;

std::vector<BackendEvent> runOneCodexTurnAgainst(const std::vector<std::string>& lines, int exit_code, bool resuming) {
  const std::string cli = writeFakeCliScript(lines, exit_code);
  EXPECT_FALSE(cli.empty()) << "could not create the fake CLI script";
  ToolRegistry reg;
  TurnTools tools;
  tools.registry = &reg;
  tools.invoke = [](const std::string&, const nlohmann::json&) {
    return assistant_agent::ToolResult::failure("no tool runs in this test");
  };
  auto memory = std::make_shared<HarnessMemory>();
  if (resuming) {
    memory->session_id = "sess-gone";
  }
  CodexBackend backend(cli, "", memory);
  std::vector<BackendEvent> events;
  backend.sendUserMessage("hi", tools, [&](BackendEvent e) { events.push_back(std::move(e)); });
  unlink(cli.c_str());
  return events;
}

}  // namespace

// Verbatim what the CLI does for `resume <id>` when it has no such thread:
// exit 1, stdout completely empty (the reason is on stderr only, discarded).
TEST(CodexBackendResume, ResumingWithEmptyStdoutAndExit1MeansTheThreadIsGone) {
  const auto events = runOneCodexTurnAgainst({}, /*exit_code=*/1, /*resuming=*/true);
  const BackendEvent* err = firstError(events);
  ASSERT_NE(err, nullptr);
  EXPECT_TRUE(err->resume_failed);
  EXPECT_NE(err->text.find("sess-gone"), std::string::npos) << "names the session Codex no longer has";
}

TEST(CodexBackendResume, FreshConversationWithEmptyStdoutAndExit1IsAnOrdinaryError) {
  const auto events = runOneCodexTurnAgainst({}, /*exit_code=*/1, /*resuming=*/false);
  const BackendEvent* err = firstError(events);
  ASSERT_NE(err, nullptr);
  EXPECT_FALSE(err->resume_failed) << "nothing was being resumed, so nothing 'can no longer be continued'";
}

TEST(CodexBackendResume, ResumingWithThreadStartedThenExit1IsAnOrdinaryError) {
  const auto events = runOneCodexTurnAgainst(
      {R"({"type":"thread.started","thread_id":"sess-gone"})"}, /*exit_code=*/1,
      /*resuming=*/true);
  const BackendEvent* err = firstError(events);
  ASSERT_NE(err, nullptr);
  EXPECT_FALSE(err->resume_failed) << "the resume itself succeeded; whatever failed came after";
}

TEST(CodexBackendSuccess, ACleanTurnEmitsToolActivityAssistantTextMetricsThenComplete) {
  const std::vector<std::string> lines = {
      R"({"type":"thread.started","thread_id":"sess-new"})",
      // A startup notice BEFORE turn.started -- must not surface as an Error.
      R"({"type":"item.completed","item":{"id":"item_x","type":"error",)"
      R"("message":"Code Mode is unavailable because code-mode host is disabled."}})",
      R"({"type":"turn.started"})",
      R"({"type":"item.started","item":{"id":"item_0","type":"mcp_tool_call","server":"pj","tool":"ping",)"
      R"("arguments":{"echo":"hello"},"result":null,"error":null,"status":"in_progress"}})",
      R"({"type":"item.completed","item":{"id":"item_0","type":"mcp_tool_call","server":"pj","tool":"ping",)"
      R"("arguments":{"echo":"hello"},"result":{"content":[{"type":"text","text":"pong: hello"}],)"
      R"("structured_content":null},"error":null,"status":"completed"}})",
      R"({"type":"item.completed","item":{"id":"item_1","type":"agent_message","text":"pong"}})",
      R"({"type":"turn.completed","usage":{"input_tokens":10275,"cached_input_tokens":0,)"
      R"("cache_write_input_tokens":0,"output_tokens":5,"reasoning_output_tokens":0}})",
  };
  auto memory = std::make_shared<HarnessMemory>();
  const auto events = [&] {
    const std::string cli = writeFakeCliScript(lines, /*exit_code=*/0);
    EXPECT_FALSE(cli.empty());
    ToolRegistry reg;
    TurnTools tools;
    tools.registry = &reg;
    tools.invoke = [](const std::string&, const nlohmann::json&) {
      return assistant_agent::ToolResult::failure("no tool runs in this test");
    };
    CodexBackend backend(cli, "", memory);
    std::vector<BackendEvent> out;
    backend.sendUserMessage("hi", tools, [&](BackendEvent e) { out.push_back(std::move(e)); });
    unlink(cli.c_str());
    return out;
  }();

  ASSERT_FALSE(events.empty());
  for (const auto& e : events) {
    EXPECT_NE(e.kind, BackendEvent::Kind::Error) << e.text;
  }

  std::vector<BackendEvent::Kind> kinds;
  for (const auto& e : events) {
    kinds.push_back(e.kind);
  }
  ASSERT_GE(kinds.size(), 4u);
  // ToolActivity (started), AssistantText (pong), Metrics, TurnComplete, in
  // that relative order -- the completed-successfully tool call carries no
  // event of its own (Ignored), so exactly one ToolActivity is expected.
  const auto tool_at = std::find(kinds.begin(), kinds.end(), BackendEvent::Kind::ToolActivity);
  const auto text_at = std::find(kinds.begin(), kinds.end(), BackendEvent::Kind::AssistantText);
  const auto metrics_at = std::find(kinds.begin(), kinds.end(), BackendEvent::Kind::Metrics);
  const auto complete_at = std::find(kinds.begin(), kinds.end(), BackendEvent::Kind::TurnComplete);
  ASSERT_NE(tool_at, kinds.end());
  ASSERT_NE(text_at, kinds.end());
  ASSERT_NE(metrics_at, kinds.end());
  ASSERT_NE(complete_at, kinds.end());
  EXPECT_LT(tool_at, text_at);
  EXPECT_LT(text_at, metrics_at);
  EXPECT_LT(metrics_at, complete_at);
  EXPECT_EQ(std::count(kinds.begin(), kinds.end(), BackendEvent::Kind::ToolActivity), 1);

  EXPECT_EQ(memory->session_id, "sess-new");
}

#endif  // POSIX

// The conversation lives in the LENT memory, not in the backend object,
// exactly like ClaudeBackend's own pin (claude_smoke_test.cpp).
TEST(CodexBackend, ConversationMemorySurvivesARebuild) {
  auto memory = std::make_shared<HarnessMemory>();
  {
    CodexBackend first("codex-never-spawned", "gpt-5-codex", memory);
    memory->session_id = "sess-42";
  }  // the backend dies here; the conversation must not

  CodexBackend second("codex-never-spawned", "gpt-5-codex", memory);
  EXPECT_EQ(memory->session_id, "sess-42");
}

// --- Live opt-in smoke ------------------------------------------------------

// POSIX-guarded like the fake-CLI cases above: it spawns the real `codex`, and
// runProcess() refuses to spawn anything off POSIX.
#if defined(__unix__) || defined(__APPLE__)
TEST(CodexSmoke, ListTopicsThroughMcp) {
  const std::optional<std::string> enabled = PJ::sdk::getEnv("ASSISTANT_CODEX_SMOKE");
  if (!enabled) {
    GTEST_SKIP() << "set ASSISTANT_CODEX_SMOKE=1 (needs a logged-in `codex` CLI) to run";
  }
  const std::optional<std::string> cli = PJ::sdk::getEnv("ASSISTANT_CODEX_CLI");
  CodexBackend backend(cli.value_or("codex"), "");
  runListTopicsSmoke(backend, "Codex");
}
#endif

}  // namespace
