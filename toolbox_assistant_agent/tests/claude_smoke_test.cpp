// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Live smoke for the Claude Code backend: spawns the real `claude` CLI wired to
// a running PlotJuggler's MCP server and checks it can call a tool and answer.
// Opt-in — runs only with ASSISTANT_CLAUDE_SMOKE=1 plus ASSISTANT_MCP_URL /
// ASSISTANT_MCP_TOKEN (needs a logged-in Claude CLI; it consumes the user's
// subscription). Skipped otherwise, so CI stays offline.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <pj_base/sdk/platform.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "claude_backend.hpp"
#include "conversation_state.hpp"  // fnv1aHex, asserted against the stored hash
#include "support/backend_test_helpers.hpp"
#include "support/file_io.hpp"
#include "system_prompt.hpp"

namespace {

using assistant_agent::BackendEvent;
using assistant_agent::ClaudeBackend;
using assistant_agent::testing::runListTopicsSmoke;

// Guarded like the fake-CLI cases further down: it spawns the real
// `claude`, and runProcess() spawns only on POSIX and Windows.
#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
TEST(ClaudeSmoke, ListTopicsThroughMcp) {
  const std::optional<std::string> enabled = PJ::sdk::getEnv("ASSISTANT_CLAUDE_SMOKE");
  if (!enabled) {
    GTEST_SKIP() << "set ASSISTANT_CLAUDE_SMOKE=1 (needs a logged-in `claude` CLI) to run";
  }
  const auto endpoint = assistant_agent::testing::liveEndpoint();
  if (!endpoint) {
    GTEST_SKIP() << assistant_agent::testing::kNoLiveEndpoint;
  }
  const std::optional<std::string> cli = PJ::sdk::getEnv("ASSISTANT_CLAUDE_CLI");
  ClaudeBackend backend(cli.value_or("claude"), "");
  runListTopicsSmoke(backend, *endpoint, "Claude");
}
#endif

}  // namespace

// The one thing about this backend that is not a preference.
//
// Headless Claude reaches the user's machine through the CLI's built-in tools —
// Bash, Read, Write, the rest — and `--tools ""` is what withholds every one of
// them. Take that flag away and this plugin's assistant can read any file the
// user can. The guarantee that this plugin is structurally incapable of
// destroying anything rests on three entries in a vector that nothing was watching.
//
// The pressure to relax it is real and will come with a good reason attached:
// letting the model actually LOOK at an exported image needs `Read`, and that is
// a genuinely useful feature. It still does not go here. A bounded MCP tool of
// ours can hand over one image; `Read` hands over the filesystem.
TEST(ClaudeBackendCommandLine, WithholdsEveryBuiltInTool) {
  const std::vector<std::string> argv =
      assistant_agent::buildClaudeArgv("/usr/bin/claude", "/tmp/mcp.json", "system", "sonnet", "");

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
// what may be called. The one entry names our MCP server, whose tools
// PlotJuggler decides, so a built-in cannot be let back in through this door.
TEST(ClaudeBackendCommandLine, AllowsOnlyTheToolsOfPlotJugglersEndpoint) {
  const std::vector<std::string> argv =
      assistant_agent::buildClaudeArgv("/usr/bin/claude", "/tmp/mcp.json", "system", "sonnet", "");

  const auto at = std::find(argv.begin(), argv.end(), "--allowedTools");
  ASSERT_NE(at, argv.end());
  ASSERT_NE(std::next(at), argv.end());
  EXPECT_EQ(*std::next(at), "mcp__pj") << "every tool of the \"pj\" MCP server, and nothing else";

  const auto config = std::find(argv.begin(), argv.end(), "--mcp-config");
  ASSERT_NE(config, argv.end());
  ASSERT_NE(std::next(config), argv.end());
  EXPECT_EQ(*std::next(config), "/tmp/mcp.json") << "the endpoint and its token come from the file, never argv";
}

// A persisted conversation resumes on its very FIRST turn after a restart: the
// dialog restores HarnessMemory::session_id from the settings store, and the
// argv builder turns any non-empty id into --resume — turn number is not part
// of the contract.
TEST(ClaudeBackendCommandLine, PersistedSessionIdResumesOnTheFirstTurn) {
  const std::vector<std::string> argv =
      assistant_agent::buildClaudeArgv("/usr/bin/claude", "/tmp/mcp.json", "system", "sonnet", "persisted-session-id");

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

  const std::string first = assistant_agent::composePayload("hi", "", "CATALOG v1", memory);
  EXPECT_EQ(first.rfind("CATALOG v1", 0), 0u);
  EXPECT_EQ(first.find("loaded data changed"), std::string::npos);
  EXPECT_NE(first.find("hi"), std::string::npos);
  EXPECT_EQ(memory.sent_catalog_hash, assistant_agent::fnv1aHex("CATALOG v1"));

  const std::string same = assistant_agent::composePayload("again", "", "CATALOG v1", memory);
  EXPECT_EQ(same, "again") << "an unchanged listing must not be re-sent";

  const std::string changed = assistant_agent::composePayload("third", "", "CATALOG v2", memory);
  EXPECT_EQ(changed.rfind("CATALOG v2", 0), 0u);
  EXPECT_NE(changed.find("(The loaded data changed; the listing above replaces the earlier one.)"), std::string::npos);
  EXPECT_EQ(memory.sent_catalog_hash, assistant_agent::fnv1aHex("CATALOG v2"));

  assistant_agent::HarnessMemory no_catalog;
  EXPECT_EQ(assistant_agent::composePayload("bare", "", "", no_catalog), "bare");
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

  const std::string first = assistant_agent::composePayload("what tabs?", "", "CATALOG v1", memory);
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
  const std::string second = assistant_agent::composePayload("and now?", "", "CATALOG v1", memory);
  EXPECT_EQ(second, "and now?");
}

// The features listing (features_digest.hpp) rides ahead of the catalog
// under its own hash: each block is re-sent only when IT changed, so loading
// a file does not re-send the plugin list and enabling a plugin does not
// re-send the loaded data.
TEST(ComposePayload, FeaturesAndCatalogHaveIndependentHashes) {
  assistant_agent::HarnessMemory memory;

  const std::string first = assistant_agent::composePayload("hi", "FEATURES v1", "CATALOG v1", memory);
  EXPECT_EQ(first, "FEATURES v1\n\nCATALOG v1\n\nhi");
  EXPECT_EQ(memory.sent_features_hash, assistant_agent::fnv1aHex("FEATURES v1"));
  EXPECT_EQ(memory.sent_catalog_hash, assistant_agent::fnv1aHex("CATALOG v1"));

  EXPECT_EQ(assistant_agent::composePayload("again", "FEATURES v1", "CATALOG v1", memory), "again");

  // Only the catalog changed: the features block stays out.
  EXPECT_EQ(
      assistant_agent::composePayload("loaded more", "FEATURES v1", "CATALOG v2", memory),
      "CATALOG v2\n\n(The loaded data changed; the listing above replaces the earlier one.)\n\nloaded more");

  // Only the features changed: re-sent with its own note, the catalog stays out.
  EXPECT_EQ(
      assistant_agent::composePayload("enabled one", "FEATURES v2", "CATALOG v2", memory),
      "FEATURES v2\n\n(The PlotJuggler features listing changed.)\n\nenabled one");
  EXPECT_EQ(memory.sent_features_hash, assistant_agent::fnv1aHex("FEATURES v2"));
  EXPECT_EQ(memory.sent_catalog_hash, assistant_agent::fnv1aHex("CATALOG v2"));

  // Both changed: both notes, features first.
  EXPECT_EQ(
      assistant_agent::composePayload("both", "FEATURES v3", "CATALOG v3", memory),
      "FEATURES v3\n\n(The PlotJuggler features listing changed.)\n\nCATALOG v3\n\n(The loaded data changed; "
      "the listing above replaces the earlier one.)\n\nboth");

  // An empty features block (no provider bound) is never sent and leaves the
  // catalog rules exactly as they were.
  assistant_agent::HarnessMemory no_features;
  EXPECT_EQ(assistant_agent::composePayload("hi", "", "CATALOG v1", no_features), "CATALOG v1\n\nhi");
  EXPECT_TRUE(no_features.sent_features_hash.empty());
}

TEST(ComposePayload, AResumeForcesBothBlocksOnceEvenIfBothHashesMatch) {
  assistant_agent::HarnessMemory memory;
  memory.sent_features_hash = assistant_agent::fnv1aHex("FEATURES v1");
  memory.sent_catalog_hash = assistant_agent::fnv1aHex("CATALOG v1");
  memory.resumed_pending = true;

  const std::string first = assistant_agent::composePayload("what tabs?", "FEATURES v1", "CATALOG v1", memory);
  EXPECT_EQ(
      first,
      "FEATURES v1\n\nCATALOG v1\n\n" + std::string(assistant_agent::kResumedConversationNote) + "\n\nwhat tabs?")
      << "the resume note rides on the catalog; an unchanged features block carries no note";
  EXPECT_FALSE(memory.resumed_pending);
  EXPECT_EQ(assistant_agent::composePayload("and now?", "FEATURES v1", "CATALOG v1", memory), "and now?");
}

TEST(ComposePayload, AResumeIsConsumedOnceEveryNonEmptyBlockHasBeenSent) {
  // Features only (no catalog this turn): the one non-empty block went out,
  // so the resume is consumed.
  assistant_agent::HarnessMemory features_only;
  features_only.resumed_pending = true;
  EXPECT_EQ(assistant_agent::composePayload("q", "FEATURES v1", "", features_only), "FEATURES v1\n\nq");
  EXPECT_FALSE(features_only.resumed_pending);

  // Catalog only (no provider bound): exactly the pre-features behaviour.
  assistant_agent::HarnessMemory catalog_only;
  catalog_only.resumed_pending = true;
  EXPECT_EQ(
      assistant_agent::composePayload("q", "", "CATALOG v1", catalog_only),
      "CATALOG v1\n\n" + std::string(assistant_agent::kResumedConversationNote) + "\n\nq");
  EXPECT_FALSE(catalog_only.resumed_pending);

  // Nothing to send: the flag waits for a turn that has a listing.
  assistant_agent::HarnessMemory nothing;
  nothing.resumed_pending = true;
  EXPECT_EQ(assistant_agent::composePayload("q", "", "", nothing), "q");
  EXPECT_TRUE(nothing.resumed_pending);
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
    (void)assistant_agent::composePayload("hello", "", "CATALOG v1", *memory);
    memory->session_id = "sess-42";
  }  // the backend dies here; the conversation must not

  assistant_agent::ClaudeBackend second("claude-never-spawned", "sonnet", memory);
  EXPECT_EQ(memory->session_id, "sess-42");
  EXPECT_EQ(memory->sent_catalog_hash, assistant_agent::fnv1aHex("CATALOG v1"));
  // The rebuilt backend composes over the same memory: an unchanged listing is
  // not re-sent, exactly as if the first backend were still alive.
  EXPECT_EQ(assistant_agent::composePayload("again", "", "CATALOG v1", *memory), "again");
}

// A PlotJuggler that cannot serve the tools fails the turn before any CLI is
// looked up, with the reason the dialog got when it acquired the endpoint.
TEST(ClaudeBackend, ATurnWithoutAnEndpointFailsWithItsReasonAndRunsNoCli) {
  assistant_agent::TurnTools tools;
  tools.mcp = PJ::unexpected("This PlotJuggler cannot serve the Assistant's tools: test");
  ClaudeBackend backend("claude-never-spawned", "");
  std::vector<BackendEvent> events;
  backend.sendUserMessage("hi", tools, [&](BackendEvent e) { events.push_back(std::move(e)); });
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, BackendEvent::Kind::Error);
  EXPECT_EQ(events[0].text, "This PlotJuggler cannot serve the Assistant's tools: test");
  EXPECT_EQ(events[1].kind, BackendEvent::Kind::TurnComplete);
}

#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
#include <memory>

namespace {

using assistant_agent::testing::firstError;
using assistant_agent::testing::removeFakeCli;
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
  const assistant_agent::TurnTools tools = assistant_agent::testing::toolsFor();
  auto memory = std::make_shared<assistant_agent::HarnessMemory>();
  if (resuming) {
    memory->session_id = "sess-gone";
  }
  assistant_agent::ClaudeBackend backend(cli, model, memory);
  std::vector<assistant_agent::BackendEvent> events;
  backend.sendUserMessage("hi", tools, [&](assistant_agent::BackendEvent e) { events.push_back(std::move(e)); });
  removeFakeCli(cli);
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

// A tool call and its answer come back as a tool_use block and, in a later user record, the tool_result that
// names its id: the backend reports the pair as a ToolActivity followed by a ToolResult naming the same tool.
TEST(ClaudeBackendToolResult, AnAnsweredCallIsAToolResultNamingTheTool) {
  for (const bool failed : {false, true}) {
    const std::string cli = writeFakeCliScript(
        {R"({"type":"system","subtype":"init","session_id":"sess-tool"})",
         R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_01","name":"mcp__pj__list_topics","input":{}}]}})",
         std::string(R"({"type":"user","message":{"content":[{"tool_use_id":"toolu_01","type":"tool_result",)") +
             (failed ? R"("is_error":true,)" : "") + R"("content":[{"type":"text","text":"{}"}]}]}})",
         R"({"type":"result","subtype":"success","is_error":false,"num_turns":2,"result":"done","session_id":"sess-tool"})"},
        /*exit_code=*/0);
    ASSERT_FALSE(cli.empty());
    const assistant_agent::TurnTools tools = assistant_agent::testing::toolsFor();
    assistant_agent::ClaudeBackend backend(cli, "", std::make_shared<assistant_agent::HarnessMemory>());
    std::vector<assistant_agent::BackendEvent> events;
    backend.sendUserMessage("hi", tools, [&](assistant_agent::BackendEvent e) { events.push_back(std::move(e)); });
    removeFakeCli(cli);

    using Kind = assistant_agent::BackendEvent::Kind;
    const auto activity =
        std::find_if(events.begin(), events.end(), [](const auto& e) { return e.kind == Kind::ToolActivity; });
    const auto result =
        std::find_if(events.begin(), events.end(), [](const auto& e) { return e.kind == Kind::ToolResult; });
    ASSERT_NE(activity, events.end());
    ASSERT_NE(result, events.end());
    EXPECT_LT(activity, result);
    EXPECT_EQ(activity->text, "list_topics");
    EXPECT_EQ(result->text, "list_topics");
    EXPECT_EQ(result->tool_failed, failed);
    EXPECT_EQ(firstError(events), nullptr);
  }
}

// The live smoke's own verdict, against the stream a CLI that got an answer prints: it passes.
TEST(ClaudeBackendToolResult, TheSmokeVerdictAcceptsAnAnsweredListTopicsCall) {
  const std::string cli = writeFakeCliScript(
      {R"({"type":"system","subtype":"init","session_id":"sess-tool"})",
       R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_01","name":"mcp__pj__list_topics","input":{}}]}})",
       R"({"type":"user","message":{"content":[{"tool_use_id":"toolu_01","type":"tool_result","content":[{"type":"text","text":"{}"}]}]}})",
       R"({"type":"result","subtype":"success","is_error":false,"num_turns":2,"result":"none","session_id":"sess-tool"})"},
      /*exit_code=*/0);
  ASSERT_FALSE(cli.empty());
  assistant_agent::ClaudeBackend backend(cli, "", std::make_shared<assistant_agent::HarnessMemory>());
  runListTopicsSmoke(backend, assistant_agent::testing::kTestEndpoint, "Claude");
  removeFakeCli(cli);
}

TEST(ClaudeBackendApiError, NamesTheConfiguredModel) {
  const auto events = runOneTurnAgainst(kApiErrorZeroTokens, /*resuming=*/false, /*model=*/"opus");
  const assistant_agent::BackendEvent* err = firstError(events);
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(
      err->text, "the Claude CLI could not reach the API with model 'opus' (unrecognized model, or the API is down)");
}
#endif

// What a real turn hands the CLI, recorded by a fake one: the composed
// payload on stdin (features block first, then the catalog, each only when
// new) and the instructions in --append-system-prompt (the system prompt plus
// the product policy, unless a caller measures without it).
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>

#include <filesystem>
#include <nlohmann/json.hpp>

namespace {

using assistant_agent::TurnTools;
using assistant_agent::testing::kTestCatalog;
using assistant_agent::testing::kTestFeatures;
using assistant_agent::testing::payloadTestTools;
using assistant_agent::testing::RecordingCliCleanup;

TEST(ClaudeBackendPayload, TheCliReceivesTheFeaturesBlockAheadOfTheCatalogAndThePolicy) {
  const std::string record = assistant_agent::testing::makeRecordPrefix("claude");
  const std::string cli = assistant_agent::testing::writeRecordingCliScript(
      {R"({"type":"system","subtype":"init","session_id":"sess-1"})",
       R"({"type":"result","subtype":"success","is_error":false,"num_turns":1,"result":"ok","session_id":"sess-1"})"},
      0, record);
  const RecordingCliCleanup cleanup({cli}, record);
  ASSERT_FALSE(cli.empty());
  const TurnTools tools = payloadTestTools();

  ClaudeBackend backend(cli, "");
  backend.sendUserMessage("hi", tools, [](BackendEvent) {});
  EXPECT_EQ(
      assistant_agent::testing::readFileToString(record + ".stdin"),
      std::string(kTestFeatures) + "\n\n" + kTestCatalog + "\n\nhi");
  const std::string first_argv = assistant_agent::testing::readFileToString(record + ".argv");
  EXPECT_NE(first_argv.find(assistant_agent::kProductPolicy), std::string::npos);
  EXPECT_EQ(first_argv.find(assistant_agent::testing::kTestEndpoint.bearer_token), std::string::npos)
      << "the token travels in the --mcp-config file, never argv";

  backend.sendUserMessage("again", tools, [](BackendEvent) {});
  EXPECT_EQ(assistant_agent::testing::readFileToString(record + ".stdin"), "again") << "neither listing changed";

  ClaudeBackend baseline(cli, "");
  baseline.setInstructions(assistant_agent::assistantInstructions(/*with_product_policy=*/false));
  baseline.sendUserMessage("hi", tools, [](BackendEvent) {});
  const std::string argv = assistant_agent::testing::readFileToString(record + ".argv");
  EXPECT_EQ(argv.find(assistant_agent::kProductPolicy), std::string::npos);
  EXPECT_NE(argv.find(assistant_agent::kSystemPrompt), std::string::npos);
}

// A first turn whose CLI exits 1 before saying anything (not logged in, say)
// never reached the model, so the retry must carry both listings again.
TEST(ClaudeBackendPayload, AFailedTurnThatProducedNoAnswerLeavesTheListingsUnsent) {
  const std::string record = assistant_agent::testing::makeRecordPrefix("claude_retry");
  const std::string failing = assistant_agent::testing::writeRecordingCliScript({}, 1, record);
  const std::string working = assistant_agent::testing::writeRecordingCliScript(
      {R"({"type":"system","subtype":"init","session_id":"sess-1"})",
       R"({"type":"result","subtype":"success","is_error":false,"num_turns":1,"result":"ok","session_id":"sess-1"})"},
      0, record);
  const RecordingCliCleanup cleanup({failing, working}, record);
  ASSERT_FALSE(failing.empty());
  ASSERT_FALSE(working.empty());
  const TurnTools tools = payloadTestTools();
  const std::string both = std::string(kTestFeatures) + "\n\n" + kTestCatalog + "\n\n";
  auto memory = std::make_shared<assistant_agent::HarnessMemory>();
  memory->resumed_pending = true;

  std::vector<BackendEvent> events;
  ClaudeBackend(failing, "", memory).sendUserMessage("hi", tools, [&](BackendEvent e) {
    events.push_back(std::move(e));
  });
  ASSERT_NE(firstError(events), nullptr);
  EXPECT_TRUE(memory->sent_features_hash.empty());
  EXPECT_TRUE(memory->sent_catalog_hash.empty());
  EXPECT_TRUE(memory->resumed_pending) << "the resume note is still owed";

  ClaudeBackend backend(working, "", memory);
  backend.sendUserMessage("hi again", tools, [](BackendEvent) {});
  const std::string sent = assistant_agent::testing::readFileToString(record + ".stdin");
  EXPECT_EQ(sent.rfind(both, 0), 0u) << sent;
  EXPECT_NE(sent.find(assistant_agent::kResumedConversationNote), std::string::npos) << sent;
  EXPECT_FALSE(memory->resumed_pending);

  // A successful turn is never rolled back, even one with no assistant text.
  backend.sendUserMessage("third", tools, [](BackendEvent) {});
  EXPECT_EQ(assistant_agent::testing::readFileToString(record + ".stdin"), "third");
}

// The --mcp-config path a recorded argv (one entry per line) names.
std::string mcpConfigPathFrom(const std::string& argv) {
  const std::string key = "--mcp-config\n";
  const std::size_t at = argv.find(key);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t begin = at + key.size();
  return argv.substr(begin, argv.find('\n', begin) - begin);
}

// The token reaches the CLI only through the --mcp-config file: owner-only,
// reused while the endpoint is unchanged, replaced when it changes, and
// removed with the backend.
TEST(ClaudeBackendPayload, TheMcpConfigFileIsPrivateCarriesTheTokenAndGoesWithTheBackend) {
  const std::string record = assistant_agent::testing::makeRecordPrefix("claude_mcp");
  const std::string cli = assistant_agent::testing::writeRecordingCliScript(
      {R"({"type":"result","subtype":"success","is_error":false,"num_turns":1,"result":"ok","session_id":"sess-1"})"},
      0, record);
  const RecordingCliCleanup cleanup({cli}, record);
  ASSERT_FALSE(cli.empty());
  const PJ::sdk::McpEndpoint& endpoint = assistant_agent::testing::kTestEndpoint;
  const auto config_path = [&record] {
    return mcpConfigPathFrom(assistant_agent::testing::readFileToString(record + ".argv"));
  };

  std::string first;
  std::string second;
  {
    ClaudeBackend backend(cli, "");
    backend.sendUserMessage("hi", payloadTestTools(), [](BackendEvent) {});
    first = config_path();
    ASSERT_FALSE(first.empty());
    const nlohmann::json cfg = nlohmann::json::parse(assistant_agent::testing::readFileToString(first));
    const nlohmann::json& pj = cfg.at("mcpServers").at("pj");
    EXPECT_EQ(pj.at("type"), "http");
    EXPECT_EQ(pj.at("url"), endpoint.url);
    EXPECT_EQ(pj.at("headers").at("Authorization"), "Bearer " + endpoint.bearer_token);
    struct stat st{};
    ASSERT_EQ(stat(first.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600) << "the token is a credential: owner-only";

    backend.sendUserMessage("again", payloadTestTools(), [](BackendEvent) {});
    EXPECT_EQ(config_path(), first) << "the same endpoint reuses its file";

    backend.sendUserMessage(
        "re-acquired", assistant_agent::testing::toolsFor({endpoint.url, "token-2"}), [](BackendEvent) {});
    second = config_path();
    ASSERT_FALSE(second.empty());
    EXPECT_NE(second, first);
    EXPECT_FALSE(std::filesystem::exists(first)) << "the replaced file is unlinked";
    EXPECT_NE(assistant_agent::testing::readFileToString(second).find("Bearer token-2"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(second)) << "the destructor unlinks the file";
}

}  // namespace
#endif
