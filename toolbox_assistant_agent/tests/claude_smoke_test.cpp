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
#include <iterator>
#include <string>
#include <vector>

#include "claude_backend.hpp"
#include "conversation_state.hpp"  // fnv1aHex, asserted against the stored hash
#include "support/backend_test_helpers.hpp"
#include "tool_registry.hpp"

namespace {

using assistant_agent::BackendEvent;
using assistant_agent::ClaudeBackend;
using assistant_agent::ToolRegistry;
using assistant_agent::testing::runListTopicsSmoke;

TEST(ClaudeSmoke, ListTopicsThroughMcp) {
  if (std::getenv("ASSISTANT_CLAUDE_SMOKE") == nullptr) {
    GTEST_SKIP() << "set ASSISTANT_CLAUDE_SMOKE=1 (needs a logged-in `claude` CLI) to run";
  }
  const char* cli = std::getenv("ASSISTANT_CLAUDE_CLI");
  ClaudeBackend backend(cli != nullptr ? cli : "claude", "");
  runListTopicsSmoke(backend, "Claude");
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

// A persisted conversation resumes on its very FIRST turn after a restart: the
// dialog restores HarnessMemory::session_id from the settings store, and the
// argv builder turns any non-empty id into --resume — turn number is not part
// of the contract.
TEST(ClaudeBackendCommandLine, PersistedSessionIdResumesOnTheFirstTurn) {
  ToolRegistry registry;
  const std::vector<std::string> argv = assistant_agent::buildClaudeArgv(
      "/usr/bin/claude", "/tmp/mcp.json", assistant_agent::allowedToolsArg(registry), "system", "sonnet",
      "persisted-session-id");

  const auto at = std::find(argv.begin(), argv.end(), "--resume");
  ASSERT_NE(at, argv.end());
  ASSERT_NE(std::next(at), argv.end());
  EXPECT_EQ(*std::next(at), "persisted-session-id");
}

// The catalog-note rules, hash-based so only the hash needs persisting: first
// send carries the listing without a note; a changed listing carries the note;
// an unchanged one sends the bare text.
TEST(ClaudeComposePayload, CatalogNoteRules) {
  assistant_agent::HarnessMemory memory;

  const std::string first = assistant_agent::composePayload("hi", "CATALOG v1", memory);
  EXPECT_EQ(first.rfind("CATALOG v1", 0), 0u);
  EXPECT_EQ(first.find("loaded data changed"), std::string::npos);
  EXPECT_NE(first.find("hi"), std::string::npos);
  EXPECT_EQ(memory.sent_catalog_hash, assistant_agent::fnv1aHex("CATALOG v1"));

  const std::string same = assistant_agent::composePayload("again", "CATALOG v1", memory);
  EXPECT_EQ(same, "again") << "an unchanged listing must not be re-sent";

  const std::string changed = assistant_agent::composePayload("third", "CATALOG v2", memory);
  EXPECT_EQ(changed.rfind("CATALOG v2", 0), 0u);
  EXPECT_NE(changed.find("(The loaded data changed; the listing above replaces the earlier one.)"), std::string::npos);
  EXPECT_EQ(memory.sent_catalog_hash, assistant_agent::fnv1aHex("CATALOG v2"));

  assistant_agent::HarnessMemory no_catalog;
  EXPECT_EQ(assistant_agent::composePayload("bare", "", no_catalog), "bare");
  EXPECT_TRUE(no_catalog.sent_catalog_hash.empty());
}

// switchToConversation (assistant_dialog.cpp) sets resumed_pending after
// pointing a fresh HarnessMemory at a conversation resumed off disk. The next
// turn must re-send the catalog with the RESUME note, even in the (unlikely
// but possible, e.g. a hash collision or a memory the caller pre-seeded)
// case where sent_catalog_hash already equals this catalog's hash -- and
// must do so exactly once, not on every later turn.
TEST(ClaudeComposePayload, ResumedPendingForcesTheResumeNoteOnceEvenIfTheHashAlreadyMatches) {
  assistant_agent::HarnessMemory memory;
  memory.sent_catalog_hash = assistant_agent::fnv1aHex("CATALOG v1");  // as if already sent
  memory.resumed_pending = true;

  const std::string first = assistant_agent::composePayload("what tabs?", "CATALOG v1", memory);
  EXPECT_EQ(first.rfind("CATALOG v1", 0), 0u) << "resuming must re-send the catalog despite the matching hash";
  EXPECT_NE(
      first.find(
          "(Resumed conversation. The tabs you composed earlier may no longer exist; plot_tab with action list reports "
          "the ones that do. The listing above is the data "
          "loaded now.)"),
      std::string::npos);
  EXPECT_EQ(first.find("loaded data changed"), std::string::npos) << "the resume note replaces the changed note";
  EXPECT_NE(first.find("what tabs?"), std::string::npos);
  EXPECT_FALSE(memory.resumed_pending) << "consumed after the first send that actually carries it";

  // The very next turn, with the same (now-current) catalog: an ordinary
  // unchanged-listing turn, no note at all.
  const std::string second = assistant_agent::composePayload("and now?", "CATALOG v1", memory);
  EXPECT_EQ(second, "and now?");
}

// The conversation lives in the LENT memory, not in the backend object: a
// settings change destroys and rebuilds the backend, and must not reset what
// was said. Constructing a backend over a memory that already holds a
// conversation is the moment a careless constructor would wipe it — this pin
// used to live in the Ollama backend's rebuild test, the only offline coverage
// of the invariant until that backend was retired.
TEST(ClaudeBackend, ConversationMemorySurvivesARebuild) {
  auto memory = std::make_shared<assistant_agent::HarnessMemory>();
  {
    assistant_agent::ClaudeBackend first("claude-never-spawned", "sonnet", memory);
    // What a completed turn leaves behind, written where the worker writes it.
    (void)assistant_agent::composePayload("hello", "CATALOG v1", *memory);
    memory->session_id = "sess-42";
  }  // the backend dies here; the conversation must not

  assistant_agent::ClaudeBackend second("claude-never-spawned", "sonnet", memory);
  EXPECT_EQ(memory->session_id, "sess-42");
  EXPECT_EQ(memory->sent_catalog_hash, assistant_agent::fnv1aHex("CATALOG v1"));
  // The rebuilt backend composes over the same memory: an unchanged listing is
  // not re-sent, exactly as if the first backend were still alive.
  EXPECT_EQ(assistant_agent::composePayload("again", "CATALOG v1", *memory), "again");
}

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>

#include <memory>

namespace {

using assistant_agent::testing::firstError;
using assistant_agent::testing::writeFakeCliScript;

// A stand-in for the CLI that drains its stdin and prints a canned
// stream-json exchange. Offline: no model, no subscription — this pins how the
// backend TRANSLATES what the CLI reports, not what the CLI does. A thin
// wrapper over the shared writeFakeCliScript: Claude's fake CLI always emits
// the init record first, then exits 1.
std::string writeFakeCli(const std::string& result_line) {
  return writeFakeCliScript(
      {R"({"type":"system","subtype":"init","session_id":"sess-gone"})", result_line}, /*exit_code=*/1);
}

std::vector<assistant_agent::BackendEvent> runOneTurnAgainst(
    const std::string& result_line, bool resuming, const std::string& model = "") {
  const std::string cli = writeFakeCli(result_line);
  EXPECT_FALSE(cli.empty()) << "could not create the fake CLI script";
  assistant_agent::ToolRegistry reg;
  assistant_agent::TurnTools tools;
  tools.registry = &reg;
  tools.invoke = [](const std::string&, const nlohmann::json&) {
    return assistant_agent::ToolResult::failure("no tool runs in this test");
  };
  auto memory = std::make_shared<assistant_agent::HarnessMemory>();
  if (resuming) {
    memory->session_id = "sess-gone";
  }
  assistant_agent::ClaudeBackend backend(cli, model, memory);
  std::vector<assistant_agent::BackendEvent> events;
  backend.sendUserMessage("hi", tools, [&](assistant_agent::BackendEvent e) { events.push_back(std::move(e)); });
  unlink(cli.c_str());
  return events;
}

constexpr const char* kZeroTurnError =
    R"({"type":"result","subtype":"error_during_execution","is_error":true,"num_turns":0,"result":null,"session_id":"sess-gone"})";
constexpr const char* kLaterError =
    R"({"type":"result","subtype":"error_during_execution","is_error":true,"num_turns":2,"result":"boom","session_id":"sess-gone"})";

// Verbatim (trimmed) shape of the real line this build was verified against:
// an unrecognized --model value, with the CLI exiting nonzero on top (the fake
// script always does — see writeFakeCli) to prove the generic "exited N"
// fallback stays suppressed.
constexpr const char* kApiErrorZeroTokens =
    R"({"duration_api_ms":0,"stop_reason":"stop_sequence","session_id":"59c13c03-bd19-4994-a4d0-ca9016a2d7ae",)"
    R"("total_cost_usd":0,"usage":{"input_tokens":0,"cache_creation_input_tokens":0,)"
    R"("cache_read_input_tokens":0,"output_tokens":0},"terminal_reason":"api_error"})";

}  // namespace

// Verbatim what the CLI emits for `--resume <id>` when it has no such session
// (stderr says "No conversation found with session ID: …", but stderr is
// discarded): an error with zero turns. That, and only that, is a failed
// resume — the one failure where retrying the same turn cannot help.
TEST(ClaudeBackendResume, AZeroTurnErrorWhileResumingMeansTheSessionIsGone) {
  const auto events = runOneTurnAgainst(kZeroTurnError, /*resuming=*/true);
  const assistant_agent::BackendEvent* err = firstError(events);
  ASSERT_NE(err, nullptr);
  EXPECT_TRUE(err->resume_failed);
  EXPECT_NE(err->text.find("sess-gone"), std::string::npos) << "names the session the CLI no longer has";
}

TEST(ClaudeBackendResume, AZeroTurnErrorOnAFreshConversationIsAnOrdinaryError) {
  const auto events = runOneTurnAgainst(kZeroTurnError, /*resuming=*/false);
  const assistant_agent::BackendEvent* err = firstError(events);
  ASSERT_NE(err, nullptr);
  EXPECT_FALSE(err->resume_failed) << "nothing was being resumed, so nothing 'can no longer be continued'";
}

TEST(ClaudeBackendResume, AnErrorAfterTurnsRanIsNotAResumeFailure) {
  const auto events = runOneTurnAgainst(kLaterError, /*resuming=*/true);
  const assistant_agent::BackendEvent* err = firstError(events);
  ASSERT_NE(err, nullptr);
  EXPECT_FALSE(err->resume_failed) << "the session resumed fine; the model failed later";
  EXPECT_EQ(err->text, "boom") << "the CLI's own wording is kept";
}

// terminal_reason=="api_error" paired with an all-zero usage block means the
// CLI never reached the API at all -- neither is_error nor subtype says so on
// this real line, so this is the one case that is not detected through the
// ordinary `ev.is_error` branch.
TEST(ClaudeBackendApiError, NamesCliDefaultWhenNoModelIsConfiguredAndSuppressesTheExitLine) {
  const auto events = runOneTurnAgainst(kApiErrorZeroTokens, /*resuming=*/false, /*model=*/"");
  const assistant_agent::BackendEvent* err = firstError(events);
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(
      err->text,
      "the Claude CLI could not reach the API with model 'CLI default' (unrecognized model, or the API is down)");

  int error_count = 0;
  for (const auto& e : events) {
    if (e.kind == assistant_agent::BackendEvent::Kind::Error) {
      ++error_count;
    }
  }
  EXPECT_EQ(error_count, 1) << "the generic 'exited N' fallback must not ALSO fire (fake CLI exits 1)";
}

TEST(ClaudeBackendApiError, NamesTheConfiguredModel) {
  const auto events = runOneTurnAgainst(kApiErrorZeroTokens, /*resuming=*/false, /*model=*/"opus");
  const assistant_agent::BackendEvent* err = firstError(events);
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(
      err->text, "the Claude CLI could not reach the API with model 'opus' (unrecognized model, or the API is down)");
}
#endif
