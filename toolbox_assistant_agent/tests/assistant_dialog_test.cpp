// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The conversations drawer end to end, against a REAL ClaudeBackend pointed
// at a throwaway $HOME: two of tests/fixtures/sessions/'s files are copied
// into the harness store that ClaudeBackend would read from
// (~/.claude/projects/<slug>/), so listConversations/loadTranscript/
// deleteConversation exercise the real claude_sessions.hpp parser, not a
// mock. None of the paths this file drives (conversationList's selection,
// delete, rename) ever call sendUserMessage or testConnection, so no `claude`
// CLI is spawned — this stays fully offline. The conversations drawer is
// always visible now (no menuButton toggle), so a refresh happens on bind,
// on a backend switch, and after a delete -- never behind an "open" click.
#include "assistant_dialog.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>  // tzset
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/settings_store_host.hpp>
#include <string>
#include <vector>

#include "claude_sessions.hpp"
#include "codex_sessions.hpp"
#include "conversation_state.hpp"
#include "settings_store.hpp"
#include "support/scoped_env.hpp"

#ifndef ASSISTANT_SESSIONS_FIXTURES_DIR
#error "ASSISTANT_SESSIONS_FIXTURES_DIR must be defined by CMake"
#endif

namespace {

using assistant_agent::AssistantDialog;
using assistant_agent::claudeSessionsDir;
using assistant_agent::loadActiveSessionId;
using assistant_agent::loadConversationTitles;
using assistant_agent::SettingsStore;
using nlohmann::json;

using assistant_agent::testing::makeTempDir;
using assistant_agent::testing::ScopedEnv;

// Mirrors ClaudeBackend::ensureWorkDir's own resolution (claude_backend.cpp):
// $XDG_STATE_HOME (unset here) else $HOME/.local/state, then the fixed
// "pj-assistant-cli" leaf.
std::filesystem::path expectedWorkDir(const std::filesystem::path& home) {
  return home / ".local/state/pj-assistant-cli";
}

// A fresh $HOME per test, with two fixtures pre-seeded into the exact
// directory a real ClaudeBackend running under that $HOME would read its
// conversations from.
class AssistantDialogDrawerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    home_ = makeTempDir("assistant_dialog_test_");
    ASSERT_FALSE(home_.empty());
    home_env_ = std::make_unique<ScopedEnv>("HOME", home_.c_str());
    xdg_env_ = std::make_unique<ScopedEnv>("XDG_STATE_HOME", nullptr);
    cfg_env_ = std::make_unique<ScopedEnv>("CLAUDE_CONFIG_DIR", nullptr);
    // The drawer stamps each row with the conversation's LOCAL time; pin the
    // zone so the row texts asserted below do not move with the machine.
    tz_env_ = std::make_unique<ScopedEnv>("TZ", "UTC");
    tzset();

    // ClaudeBackend::ensureWorkDir mkdir()s only the LAST path component,
    // trusting "$HOME/.local/state" to already exist (true on any real
    // desktop; comment there: "XDG_STATE_HOME normally exists"). A bare
    // mkdtemp() $HOME has neither, so create the parent ourselves -- this
    // mirrors reality, it isn't a production code change.
    std::error_code ec;
    std::filesystem::create_directories(home_ / ".local/state", ec);
    ASSERT_FALSE(ec) << (home_ / ".local/state") << ": " << ec.message();

    const std::filesystem::path sessions_dir = claudeSessionsDir(expectedWorkDir(home_).string());
    std::filesystem::create_directories(sessions_dir, ec);
    ASSERT_FALSE(ec) << sessions_dir << ": " << ec.message();
    for (const char* name : {"session_alpha.jsonl", "session_gamma.jsonl"}) {
      std::filesystem::copy_file(
          std::filesystem::path(ASSISTANT_SESSIONS_FIXTURES_DIR) / name, sessions_dir / name, ec);
      ASSERT_FALSE(ec) << name << ": " << ec.message();
    }

    settings_view_ = PJ::sdk::SettingsView{host_.view()};
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(home_, ec);
  }

  // widget_data() drains its dirty flags on every call, so a checkpoint that
  // wants to inspect more than one widget must snapshot it exactly ONCE and
  // read every field from that same document -- a second call after a state
  // change but with nothing newly dirty returns "{}", not the prior state.
  json snapshot(AssistantDialog& dialog) {
    const std::string raw = dialog.widget_data();
    if (raw.empty()) {
      return json::object();
    }
    const json doc = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    return doc.is_object() ? doc : json::object();
  }
  static json entryIn(const json& snapshot, const std::string& widget) {
    return snapshot.value(widget, json::object());
  }

  std::filesystem::path home_;
  std::unique_ptr<ScopedEnv> home_env_;
  std::unique_ptr<ScopedEnv> xdg_env_;
  std::unique_ptr<ScopedEnv> cfg_env_;
  std::unique_ptr<ScopedEnv> tz_env_;
  PJ::sdk::InMemorySettingsBackend backend_;
  PJ::sdk::SettingsStoreHost host_{backend_};
  PJ::sdk::SettingsView settings_view_;
};

TEST_F(AssistantDialogDrawerTest, ThePanelListsRealHarnessConversationsFromTheFirstFrame) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);  // the drawer is always visible now -- no open click needed

  const json snap = snapshot(dialog);
  const json list = entryIn(snap, "conversationList");

  ASSERT_TRUE(list.contains("list_items"));
  const std::vector<std::string> items = list.at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 2u);
  // session_gamma (no ai-title -> falls back to the first prompt, truncated)
  // is newer than session_alpha (ai-title "PlotJuggler demo") -- newest first.
  EXPECT_EQ(items[0], "Can you plot the vehicle speed against the steer · 2 Sep 09:00");
  EXPECT_EQ(items[1], "PlotJuggler demo · 1 Sep 15:17");
}

TEST_F(AssistantDialogDrawerTest, SelectingAConversationReplaysItsTranscriptAndPersistsTheActiveId) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  (void)dialog.widget_data();  // flush the initial render before selecting

  ASSERT_TRUE(dialog.onSelectionChanged(
      "conversationList", {"Can you plot the vehicle speed against the steer · 2 Sep 09:00"}));
  const json snap = snapshot(dialog);

  const json transcript = entryIn(snap, "transcriptText");
  ASSERT_TRUE(transcript.contains("plain_text"));
  const std::string text = transcript.at("plain_text").get<std::string>();
  EXPECT_NE(text.find("You: Can you plot the vehicle speed against the steering angle?"), std::string::npos);
  EXPECT_NE(text.find("list_topics"), std::string::npos) << "the tool_use row, name only, like the live path";
  EXPECT_NE(
      text.find("Assistant: I opened a new tab plotting vehicle_speed against vehicle_steering."), std::string::npos);
  EXPECT_NE(text.find("resumed previous conversation"), std::string::npos);

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "session_gamma");
}

TEST_F(AssistantDialogDrawerTest, DeletingTheActiveConversationStartsANewOneAndRemovesTheFile) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_TRUE(dialog.onSelectionChanged(
      "conversationList", {"Can you plot the vehicle speed against the steer · 2 Sep 09:00"}));

  const json before = entryIn(snapshot(dialog), "conversationList");
  const std::vector<std::string> items_before = before.at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items_before.size(), 2u);
  ASSERT_EQ(items_before[0], "Can you plot the vehicle speed against the steer · 2 Sep 09:00")
      << "session_gamma must still be the active (and newest) row before deleting it";

  ASSERT_TRUE(dialog.onItemDeleteRequested("conversationList", 0));
  // ONE snapshot: the delete's list refresh and its startNewConversation()
  // both land in this same widget_data() build, so reading them from two
  // separate calls would have the second one see nothing newly dirty.
  const json snap = snapshot(dialog);

  const std::vector<std::string> items_after =
      entryIn(snap, "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items_after.size(), 1u);
  EXPECT_EQ(items_after[0], "PlotJuggler demo · 1 Sep 15:17");

  EXPECT_EQ(entryIn(snap, "transcriptText").value("plain_text", std::string("not-empty")), "")
      << "deleting the active conversation starts a blank new one";

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "");

  const std::filesystem::path gamma_file = claudeSessionsDir(expectedWorkDir(home_).string()) / "session_gamma.jsonl";
  EXPECT_FALSE(std::filesystem::exists(gamma_file)) << "the file itself must be gone, not just delisted";
}

TEST_F(AssistantDialogDrawerTest, ReopeningThePanelResumesThePersistedConversation) {
  // What a previous instance leaves behind: only the active id. The transcript
  // itself has to come back from the harness's store on the first bind.
  {
    SettingsStore store(settings_view_);
    saveActiveSessionId(store, "session_alpha", "claude");
  }
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  const json snap = snapshot(dialog);

  const json transcript = entryIn(snap, "transcriptText");
  ASSERT_TRUE(transcript.contains("plain_text"));
  const std::string text = transcript.at("plain_text").get<std::string>();
  EXPECT_NE(text.find("Give me a demo"), std::string::npos) << "alpha's first prompt, catalog stripped";
  EXPECT_EQ(text.find("Loaded data"), std::string::npos) << "the injected catalog is not a transcript row";
  EXPECT_NE(text.find("resumed previous conversation"), std::string::npos);
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "session_alpha");
}

TEST_F(AssistantDialogDrawerTest, APurgedActiveConversationIsForgottenOnReopen) {
  {
    SettingsStore store(settings_view_);
    saveActiveSessionId(store, "session_vanished", "claude");  // Claude's retention took the file
  }
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  const json snap = snapshot(dialog);

  EXPECT_EQ(entryIn(snap, "transcriptText").value("plain_text", std::string("not-empty")), "")
      << "nothing to replay, so the panel opens blank rather than half-resumed";
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "")
      << "a dangling --resume target must not survive to the next turn";
}

// --- Rename, via the context-menu action ("conversationList", "rename") ---

TEST_F(AssistantDialogDrawerTest, RenamingTheActiveRowKeepsItActiveAndLeavesTheTranscriptAlone) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_TRUE(dialog.onSelectionChanged(
      "conversationList", {"Can you plot the vehicle speed against the steer · 2 Sep 09:00"}));
  (void)dialog.widget_data();  // flush the selection's render before renaming

  // Row 0 is still session_gamma: selecting it doesn't reorder the list.
  ASSERT_TRUE(dialog.onItemContextAction("conversationList", 0, "rename"));
  (void)dialog.widget_data();  // let the render request the sub-dialog (sets open_sub_dialog)
  EXPECT_FALSE(dialog.onTextChanged("renameEdit", "Steering demo"));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  const json snap = snapshot(dialog);
  EXPECT_FALSE(snap.contains("transcriptText")) << "a rename must not re-render the transcript";
  const std::vector<std::string> items =
      entryIn(snap, "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0], "Steering demo · 2 Sep 09:00")
      << "the custom name replaces the harness title; the date suffix stays";
  EXPECT_EQ(items[1], "PlotJuggler demo · 1 Sep 15:17");

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "session_gamma")
      << "renaming must not change which conversation is active";
}

TEST_F(AssistantDialogDrawerTest, CustomNameSurvivesReopeningThePanelAndWinsOverTheHarnessTitle) {
  {
    AssistantDialog dialog;
    dialog.setSettings(settings_view_);
    ASSERT_TRUE(dialog.onItemContextAction("conversationList", 0, "rename"));  // row 0: session_gamma
    (void)dialog.widget_data();  // let the render request the sub-dialog (sets open_sub_dialog)
    EXPECT_FALSE(dialog.onTextChanged("renameEdit", "Steering demo"));
    EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  }  // dialog destroyed here, like closing and reopening the panel

  AssistantDialog dialog2;
  dialog2.setSettings(settings_view_);
  const std::vector<std::string> items =
      entryIn(snapshot(dialog2), "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0], "Steering demo · 2 Sep 09:00");
}

TEST_F(AssistantDialogDrawerTest, AcceptingAnEmptyRenameRemovesTheCustomNameAndTheHarnessTitleReturns) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_TRUE(dialog.onItemContextAction("conversationList", 0, "rename"));
  (void)dialog.widget_data();  // let the render request the sub-dialog (sets open_sub_dialog)
  EXPECT_FALSE(dialog.onTextChanged("renameEdit", "Steering demo"));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  ASSERT_EQ(
      entryIn(snapshot(dialog), "conversationList").at("list_items").get<std::vector<std::string>>()[0],
      "Steering demo · 2 Sep 09:00");

  ASSERT_TRUE(dialog.onItemContextAction("conversationList", 0, "rename"));
  (void)dialog.widget_data();  // let the render request the sub-dialog (sets open_sub_dialog)
  EXPECT_FALSE(dialog.onTextChanged("renameEdit", "   "));  // whitespace-only
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  const std::vector<std::string> items =
      entryIn(snapshot(dialog), "conversationList").at("list_items").get<std::vector<std::string>>();
  EXPECT_EQ(items[0], "Can you plot the vehicle speed against the steer · 2 Sep 09:00")
      << "an empty (or whitespace-only) name removes the custom title; the harness title comes back";
}

TEST_F(AssistantDialogDrawerTest, PruningDropsACustomNameWhoseConversationIsGoneFromTheListing) {
  {
    AssistantDialog dialog;
    dialog.setSettings(settings_view_);
    ASSERT_TRUE(dialog.onItemContextAction("conversationList", 0, "rename"));  // row 0: session_gamma
    (void)dialog.widget_data();  // let the render request the sub-dialog (sets open_sub_dialog)
    EXPECT_FALSE(dialog.onTextChanged("renameEdit", "Steering demo"));
    EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  }

  // Simulate the harness's own retention purging the file -- not our delete
  // path, so this proves the prune is driven by the LISTING, not by onItemDeleteRequested.
  std::error_code ec;
  std::filesystem::remove(claudeSessionsDir(expectedWorkDir(home_).string()) / "session_gamma.jsonl", ec);
  ASSERT_FALSE(ec);

  AssistantDialog dialog2;
  dialog2.setSettings(settings_view_);  // the bind's refresh prunes the now-dangling title
  const std::vector<std::string> items =
      entryIn(snapshot(dialog2), "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0], "PlotJuggler demo · 1 Sep 15:17");

  // Gone from the store too, not merely unused -- otherwise a later
  // conversation reusing the same id would inherit the pruned name.
  EXPECT_TRUE(loadConversationTitles(SettingsStore(settings_view_), "claude").empty());
}

TEST_F(AssistantDialogDrawerTest, RenameAndSettingsSubDialogsRouteIndependently) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);

  // Opening the rename dialog and accepting it must not write any settings key.
  ASSERT_TRUE(dialog.onItemContextAction("conversationList", 0, "rename"));
  (void)dialog.widget_data();  // let the render request the sub-dialog (sets open_sub_dialog)
  EXPECT_FALSE(dialog.onTextChanged("renameEdit", "Steering demo"));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  EXPECT_FALSE(SettingsStore(settings_view_).contains("assistant.backend"))
      << "renaming must never touch a settings key";

  // Opening settings and accepting it still commits it, exactly as before.
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 0));  // stays on claude
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  EXPECT_EQ(SettingsStore(settings_view_).getString("assistant.backend", "unset"), "claude");
}

// --- Codex drawer cases: same behavior, different harness store -----------
//
// codex_sessions.hpp filters by the `payload.cwd` FIELD inside each rollout
// file, not by which directory it lives in (unlike Claude's per-cwd project
// directories) — so, unlike the Claude fixtures above (copied verbatim), the
// fixtures here are written by this test with `cwd` set to whatever this
// test's throwaway $HOME actually resolves CodexBackend's work dir to.

// Writes a minimal, valid `codex exec` rollout file: one session_meta record
// plus one user/assistant response_item pair — everything listCodexConversations
// / loadCodexTranscript (codex_sessions.cpp) look at.
void writeCodexFixture(
    const std::filesystem::path& path, const std::string& id, const std::string& cwd, const std::string& first_ts,
    const std::string& user_text, const std::string& assistant_text, const std::string& last_ts) {
  std::ofstream file(path);
  file << json{
              {"timestamp", first_ts},
              {"ordinal", 0},
              {"type", "session_meta"},
              {"payload",
               {{"session_id", id},
                {"id", id},
                {"timestamp", first_ts},
                {"cwd", cwd},
                {"originator", "codex_exec"},
                {"cli_version", "0.153.0"},
                {"source", "exec"}}},
          }
                .dump()
       << "\n";
  file << json{
              {"timestamp", first_ts},
              {"ordinal", 1},
              {"type", "response_item"},
              {"payload",
               {{"type", "message"},
                {"role", "user"},
                {"content", json::array({{{"type", "input_text"}, {"text", user_text}}})}}},
          }
                .dump()
       << "\n";
  file << json{
              {"timestamp", last_ts},
              {"ordinal", 2},
              {"type", "response_item"},
              {"payload",
               {{"type", "message"},
                {"role", "assistant"},
                {"content", json::array({{{"type", "output_text"}, {"text", assistant_text}}})}}},
          }
                .dump()
       << "\n";
}

class AssistantDialogCodexDrawerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    home_ = makeTempDir("assistant_dialog_codex_test_");
    ASSERT_FALSE(home_.empty());
    home_env_ = std::make_unique<ScopedEnv>("HOME", home_.c_str());
    xdg_env_ = std::make_unique<ScopedEnv>("XDG_STATE_HOME", nullptr);
    cfg_env_ = std::make_unique<ScopedEnv>("CLAUDE_CONFIG_DIR", nullptr);
    codex_home_ = makeTempDir("assistant_dialog_codex_home_");
    ASSERT_FALSE(codex_home_.empty());
    codex_home_env_ = std::make_unique<ScopedEnv>("CODEX_HOME", codex_home_.c_str());
    tz_env_ = std::make_unique<ScopedEnv>("TZ", "UTC");
    tzset();

    std::error_code ec;
    std::filesystem::create_directories(home_ / ".local/state", ec);
    ASSERT_FALSE(ec) << (home_ / ".local/state") << ": " << ec.message();

    // CodexBackend's work dir resolves through the SAME harness_workdir.hpp
    // rules as Claude's (see expectedWorkDir above).
    const std::string work_dir = expectedWorkDir(home_).string();
    const std::filesystem::path sessions_dir = codex_home_ / "sessions" / "2026" / "09" / "03";
    std::filesystem::create_directories(sessions_dir, ec);
    ASSERT_FALSE(ec) << sessions_dir << ": " << ec.message();

    writeCodexFixture(
        sessions_dir / "rollout-2026-09-01T15-17-30-codex_alpha.jsonl", "codex_alpha", work_dir,
        "2026-09-01T15:17:30.000Z", "Give me a demo showing me everything you're capable of doing inside PlotJuggler.",
        "I'd love to give you a demo of what I can do.", "2026-09-01T15:17:35.000Z");
    writeCodexFixture(
        sessions_dir / "rollout-2026-09-02T09-00-00-codex_gamma.jsonl", "codex_gamma", work_dir,
        "2026-09-02T09:00:00.000Z", "Can you plot the vehicle speed against the steering angle?",
        "I opened a new tab plotting vehicle_speed against vehicle_steering.", "2026-09-02T09:00:05.000Z");

    settings_view_ = PJ::sdk::SettingsView{host_.view()};
    // Codex active from the very first bind, so rebuildBackend() builds a
    // CodexBackend (not the ctor's default ClaudeBackend) before anything
    // tries to resume a conversation through it.
    SettingsStore(settings_view_).setString("assistant.backend", "codex");
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(home_, ec);
    std::filesystem::remove_all(codex_home_, ec);
  }

  json snapshot(AssistantDialog& dialog) {
    const std::string raw = dialog.widget_data();
    if (raw.empty()) {
      return json::object();
    }
    const json doc = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    return doc.is_object() ? doc : json::object();
  }
  static json entryIn(const json& snapshot, const std::string& widget) {
    return snapshot.value(widget, json::object());
  }

  std::filesystem::path home_;
  std::filesystem::path codex_home_;
  std::unique_ptr<ScopedEnv> home_env_;
  std::unique_ptr<ScopedEnv> xdg_env_;
  std::unique_ptr<ScopedEnv> cfg_env_;
  std::unique_ptr<ScopedEnv> codex_home_env_;
  std::unique_ptr<ScopedEnv> tz_env_;
  PJ::sdk::InMemorySettingsBackend backend_;
  PJ::sdk::SettingsStoreHost host_{backend_};
  PJ::sdk::SettingsView settings_view_;
};

TEST_F(AssistantDialogCodexDrawerTest, OpeningTheDrawerListsCodexConversationsNewestFirst) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);

  const json snap = snapshot(dialog);
  const json list = entryIn(snap, "conversationList");
  ASSERT_TRUE(list.contains("list_items"));
  const std::vector<std::string> items = list.at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0], "Can you plot the vehicle speed against the steer · 2 Sep 09:00");
  EXPECT_EQ(items[1], "Give me a demo showing me everything you're capa · 1 Sep 15:17");
}

TEST_F(AssistantDialogCodexDrawerTest, SelectingAConversationReplaysItAndPersistsUnderTheCodexKey) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);

  ASSERT_TRUE(dialog.onSelectionChanged(
      "conversationList", {"Can you plot the vehicle speed against the steer · 2 Sep 09:00"}));
  const json snap = snapshot(dialog);

  const std::string text = entryIn(snap, "transcriptText").at("plain_text").get<std::string>();
  EXPECT_NE(text.find("You: Can you plot the vehicle speed against the steering angle?"), std::string::npos);
  EXPECT_NE(
      text.find("Assistant: I opened a new tab plotting vehicle_speed against vehicle_steering."), std::string::npos);
  EXPECT_NE(text.find("resumed previous conversation"), std::string::npos);

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "codex_gamma");
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "")
      << "switching a conversation on codex must not touch claude's persisted id";
}

TEST_F(AssistantDialogCodexDrawerTest, DeletingTheActiveConversationStartsANewOneAndRemovesTheFile) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_TRUE(dialog.onSelectionChanged(
      "conversationList", {"Can you plot the vehicle speed against the steer · 2 Sep 09:00"}));

  ASSERT_TRUE(dialog.onItemDeleteRequested("conversationList", 0));
  const json snap = snapshot(dialog);

  const std::vector<std::string> items_after =
      entryIn(snap, "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items_after.size(), 1u);
  EXPECT_EQ(items_after[0], "Give me a demo showing me everything you're capa · 1 Sep 15:17");

  EXPECT_EQ(entryIn(snap, "transcriptText").value("plain_text", std::string("not-empty")), "");
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "");

  const std::filesystem::path gamma_file =
      codex_home_ / "sessions" / "2026" / "09" / "03" / "rollout-2026-09-02T09-00-00-codex_gamma.jsonl";
  EXPECT_FALSE(std::filesystem::exists(gamma_file)) << "the file itself must be gone, not just delisted";
}

TEST_F(AssistantDialogCodexDrawerTest, ReopeningThePanelResumesThePersistedConversation) {
  {
    SettingsStore store(settings_view_);
    saveActiveSessionId(store, "codex_alpha", "codex");
  }
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  const json snap = snapshot(dialog);

  const std::string text = entryIn(snap, "transcriptText").at("plain_text").get<std::string>();
  EXPECT_NE(text.find("Give me a demo"), std::string::npos);
  EXPECT_NE(text.find("resumed previous conversation"), std::string::npos);
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "codex_alpha");
}

TEST_F(AssistantDialogCodexDrawerTest, APurgedActiveConversationIsForgottenOnReopen) {
  {
    SettingsStore store(settings_view_);
    saveActiveSessionId(store, "codex_vanished", "codex");
  }
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  const json snap = snapshot(dialog);

  EXPECT_EQ(entryIn(snap, "transcriptText").value("plain_text", std::string("not-empty")), "");
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "");
}

// --- Switching backends must not discard either one's resume point --------

TEST(AssistantDialogBackendSwitch, ClaudeCodexClaudeKeepsBothSessionIds) {
  const std::filesystem::path home = makeTempDir("assistant_dialog_switch_test_");
  ASSERT_FALSE(home.empty());
  ScopedEnv home_env("HOME", home.c_str());
  ScopedEnv xdg_env("XDG_STATE_HOME", nullptr);
  ScopedEnv cfg_env("CLAUDE_CONFIG_DIR", nullptr);
  const std::filesystem::path codex_home = makeTempDir("assistant_dialog_switch_codex_home_");
  ASSERT_FALSE(codex_home.empty());
  ScopedEnv codex_home_env("CODEX_HOME", codex_home.c_str());
  ScopedEnv tz_env("TZ", "UTC");
  tzset();

  std::error_code ec;
  std::filesystem::create_directories(home / ".local/state", ec);
  ASSERT_FALSE(ec);
  const std::string work_dir = expectedWorkDir(home).string();

  const std::filesystem::path claude_sessions_dir = claudeSessionsDir(work_dir);
  std::filesystem::create_directories(claude_sessions_dir, ec);
  ASSERT_FALSE(ec);
  std::filesystem::copy_file(
      std::filesystem::path(ASSISTANT_SESSIONS_FIXTURES_DIR) / "session_alpha.jsonl",
      claude_sessions_dir / "session_alpha.jsonl", ec);
  ASSERT_FALSE(ec);

  const std::filesystem::path codex_sessions_dir = codex_home / "sessions" / "2026" / "09" / "03";
  std::filesystem::create_directories(codex_sessions_dir, ec);
  ASSERT_FALSE(ec);
  writeCodexFixture(
      codex_sessions_dir / "rollout-2026-09-02T09-00-00-codex_gamma.jsonl", "codex_gamma", work_dir,
      "2026-09-02T09:00:00.000Z", "Can you plot the vehicle speed against the steering angle?",
      "I opened a new tab plotting vehicle_speed against vehicle_steering.", "2026-09-02T09:00:05.000Z");

  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};
  PJ::sdk::SettingsView settings_view{host.view()};
  {
    // commitSettings() probes the newly-selected backend with testConnection()
    // (a real `<cli> --version` subprocess) as its very last step. Point both
    // CLI paths at names that cannot resolve on PATH, so this test — which is
    // about session-id bookkeeping, not connectivity — never spawns the real
    // `claude` or `codex` binary this dev machine happens to have installed.
    SettingsStore store(settings_view);
    store.setString("assistant.claude.cli_path", "assistant-agent-test-missing-claude-cli");
    store.setString("assistant.codex.cli_path", "assistant-agent-test-missing-codex-cli");
  }

  AssistantDialog dialog;
  dialog.setSettings(settings_view);  // starts on "claude" (the default)

  ASSERT_TRUE(dialog.onSelectionChanged("conversationList", {"PlotJuggler demo · 1 Sep 15:17"}));
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view), "claude"), "session_alpha");

  // Switch to Codex through the real settings flow (settingsButton ->
  // backendCombo -> subDialogAccepted), the same path the UI drives.
  // activateBackendConversation() refreshes the drawer's listing
  // unconditionally, so it is already codex's own by the time of the
  // selection below.
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 1));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  ASSERT_TRUE(dialog.onSelectionChanged(
      "conversationList", {"Can you plot the vehicle speed against the steer · 2 Sep 09:00"}));
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view), "codex"), "codex_gamma");
  // Claude's id must have survived the switch untouched.
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view), "claude"), "session_alpha");

  // Switch back to Claude: both ids must still be exactly what they were.
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 0));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view), "claude"), "session_alpha");
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view), "codex"), "codex_gamma");

  std::filesystem::remove_all(home, ec);
  std::filesystem::remove_all(codex_home, ec);
}

// --- Settings commit switches the conversation, and the model selector -----
//
// Both fixtures below are prepared in SetUp() (a Claude session under the fake
// CLAUDE_CONFIG_DIR, a Codex rollout under the fake CODEX_HOME), but neither
// backend's ACTIVE id is persisted there — each test persists exactly the ids
// it needs, since which one is "already active" differs per test.

class AssistantDialogSettingsSwitchTest : public ::testing::Test {
 protected:
  void SetUp() override {
    home_ = makeTempDir("assistant_settings_switch_test_");
    ASSERT_FALSE(home_.empty());
    home_env_ = std::make_unique<ScopedEnv>("HOME", home_.c_str());
    xdg_env_ = std::make_unique<ScopedEnv>("XDG_STATE_HOME", nullptr);
    cfg_env_ = std::make_unique<ScopedEnv>("CLAUDE_CONFIG_DIR", nullptr);
    codex_home_ = makeTempDir("assistant_settings_switch_codex_home_");
    ASSERT_FALSE(codex_home_.empty());
    codex_home_env_ = std::make_unique<ScopedEnv>("CODEX_HOME", codex_home_.c_str());
    tz_env_ = std::make_unique<ScopedEnv>("TZ", "UTC");
    tzset();

    std::error_code ec;
    std::filesystem::create_directories(home_ / ".local/state", ec);
    ASSERT_FALSE(ec) << (home_ / ".local/state") << ": " << ec.message();
    const std::string work_dir = expectedWorkDir(home_).string();

    const std::filesystem::path claude_sessions_dir = claudeSessionsDir(work_dir);
    std::filesystem::create_directories(claude_sessions_dir, ec);
    ASSERT_FALSE(ec) << claude_sessions_dir << ": " << ec.message();
    std::filesystem::copy_file(
        std::filesystem::path(ASSISTANT_SESSIONS_FIXTURES_DIR) / "session_alpha.jsonl",
        claude_sessions_dir / "session_alpha.jsonl", ec);
    ASSERT_FALSE(ec) << ec.message();

    const std::filesystem::path codex_sessions_dir = codex_home_ / "sessions" / "2026" / "09" / "03";
    std::filesystem::create_directories(codex_sessions_dir, ec);
    ASSERT_FALSE(ec) << codex_sessions_dir << ": " << ec.message();
    writeCodexFixture(
        codex_sessions_dir / "rollout-2026-09-02T09-00-00-codex_gamma.jsonl", "codex_gamma", work_dir,
        "2026-09-02T09:00:00.000Z", "Can you plot the vehicle speed against the steering angle?",
        "I opened a new tab plotting vehicle_speed against vehicle_steering.", "2026-09-02T09:00:05.000Z");

    settings_view_ = PJ::sdk::SettingsView{host_.view()};
    // commitSettings() probes the newly-selected backend with testConnection()
    // as its last step (a real `<cli> --version` subprocess) -- point both CLI
    // paths at names that cannot resolve on PATH, so these tests, which are
    // about conversation/model bookkeeping, never spawn a real CLI.
    SettingsStore store(settings_view_);
    store.setString("assistant.claude.cli_path", "assistant-agent-test-missing-claude-cli");
    store.setString("assistant.codex.cli_path", "assistant-agent-test-missing-codex-cli");
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(home_, ec);
    std::filesystem::remove_all(codex_home_, ec);
  }

  json snapshot(AssistantDialog& dialog) {
    const std::string raw = dialog.widget_data();
    if (raw.empty()) {
      return json::object();
    }
    const json doc = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    return doc.is_object() ? doc : json::object();
  }
  static json entryIn(const json& snapshot, const std::string& widget) {
    return snapshot.value(widget, json::object());
  }

  std::filesystem::path home_;
  std::filesystem::path codex_home_;
  std::unique_ptr<ScopedEnv> home_env_;
  std::unique_ptr<ScopedEnv> xdg_env_;
  std::unique_ptr<ScopedEnv> cfg_env_;
  std::unique_ptr<ScopedEnv> codex_home_env_;
  std::unique_ptr<ScopedEnv> tz_env_;
  PJ::sdk::InMemorySettingsBackend backend_;
  PJ::sdk::SettingsStoreHost host_{backend_};
  PJ::sdk::SettingsView settings_view_;
};

TEST_F(AssistantDialogSettingsSwitchTest, EveryPayloadNamingTheConversationListCarriesTheDeletableFlag) {
  auto checkPayload = [](const std::string& raw) {
    if (raw.empty()) {
      return;  // "no update" -- steady state, nothing to check
    }
    const json doc = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    ASSERT_TRUE(doc.is_object());
    if (doc.contains("conversationList")) {
      EXPECT_EQ(doc.at("conversationList").value("list_deletable", false), true)
          << "a widget-data object naming conversationList without this flag turns the trash/elision delegate "
             "off -- payload: "
          << doc.dump();
    }
  };

  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  checkPayload(dialog.widget_data());  // initial render -- the drawer is always visible, so this already names it

  // A settings commit that does NOT switch backends: rebuildBackend() still
  // sets controls_dirty (the status line names the backend), but nothing
  // calls activateBackendConversation(), so drawer_dirty stays false -- this
  // is exactly the payload shape (conversationList named via setEnabled
  // alone) that used to drop the flag.
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 0));  // stays on claude
  ASSERT_TRUE(dialog.onClicked("subDialogAccepted"));
  checkPayload(dialog.widget_data());

  // A controls-only refresh with no settings and no drawer touch at all:
  // sendCurrentInput() sets transcript_dirty + controls_dirty synchronously,
  // before anything reaches the (nonexistent) CLI.
  EXPECT_FALSE(dialog.onTextChanged("inputEdit", "hello"));
  ASSERT_TRUE(dialog.onClicked("sendButton"));
  checkPayload(dialog.widget_data());
}

TEST_F(AssistantDialogSettingsSwitchTest, SwitchingBackendLoadsThatBackendsPersistedConversation) {
  {
    SettingsStore store(settings_view_);
    saveActiveSessionId(store, "session_alpha", "claude");
    saveActiveSessionId(store, "codex_gamma", "codex");
  }
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);  // starts on claude, resumes session_alpha
  const std::string initial_text = entryIn(snapshot(dialog), "transcriptText").at("plain_text").get<std::string>();
  ASSERT_NE(initial_text.find("Give me a demo"), std::string::npos) << "must start on Claude's persisted conversation";

  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 1));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  const json snap = snapshot(dialog);
  const std::string text = entryIn(snap, "transcriptText").at("plain_text").get<std::string>();
  EXPECT_NE(text.find("Can you plot the vehicle speed against the steering angle?"), std::string::npos)
      << "must have switched to codex's persisted conversation";
  EXPECT_NE(text.find("I opened a new tab plotting vehicle_speed against vehicle_steering."), std::string::npos);
  const std::size_t resumed_at = text.find("resumed previous conversation");
  const std::size_t saved_at = text.find("Settings saved (backend: codex)");
  ASSERT_NE(resumed_at, std::string::npos);
  ASSERT_NE(saved_at, std::string::npos);
  EXPECT_LT(resumed_at, saved_at) << "the note must land in the transcript it switched TO, not the one just left";

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "codex_gamma");
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "session_alpha")
      << "switching must not touch claude's persisted id";

  // activateBackendConversation() refreshed the drawer unconditionally, so
  // `snap` (above, already consumed by the transcript checks) carried codex's
  // own listing too -- no extra open/close dance needed to see it.
  const std::vector<std::string> items =
      entryIn(snap, "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 1u) << "the drawer listing must be codex's now, not claude's";
  EXPECT_EQ(items[0], "Can you plot the vehicle speed against the steer · 2 Sep 09:00");

  // Switch back to Claude: nothing lost.
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 0));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  const std::string back_text = entryIn(snapshot(dialog), "transcriptText").at("plain_text").get<std::string>();
  EXPECT_NE(back_text.find("Give me a demo"), std::string::npos);
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "session_alpha");
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "codex_gamma") << "nothing lost";
}

TEST_F(AssistantDialogSettingsSwitchTest, SwitchingToABackendWithNothingPersistedStartsBlank) {
  {
    SettingsStore store(settings_view_);
    saveActiveSessionId(store, "session_alpha", "claude");
    // Deliberately nothing persisted for codex.
  }
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_NE(
      entryIn(snapshot(dialog), "transcriptText").at("plain_text").get<std::string>().find("Give me a demo"),
      std::string::npos);

  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 1));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  const json snap = snapshot(dialog);
  const std::string text = entryIn(snap, "transcriptText").at("plain_text").get<std::string>();
  EXPECT_EQ(text.find("Give me a demo"), std::string::npos) << "must not still be showing claude's conversation";
  EXPECT_EQ(text.find("resumed previous conversation"), std::string::npos)
      << "nothing was resumed -- there is nothing persisted for codex";
  EXPECT_NE(text.find("Settings saved (backend: codex)"), std::string::npos);
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "");

  // The same `snap` already carries codex's own listing -- the drawer refresh
  // on a backend switch is unconditional now.
  const std::vector<std::string> items =
      entryIn(snap, "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 1u) << "codex's own listing (the fixture exists, just was never made ACTIVE)";
  EXPECT_EQ(items[0], "Can you plot the vehicle speed against the steer · 2 Sep 09:00");
}

TEST_F(AssistantDialogSettingsSwitchTest, ChangingOnlyTheModelKeepsTheConversation) {
  {
    SettingsStore store(settings_view_);
    saveActiveSessionId(store, "session_alpha", "claude");
  }
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  const std::string initial_text = entryIn(snapshot(dialog), "transcriptText").at("plain_text").get<std::string>();
  ASSERT_NE(initial_text.find("Give me a demo"), std::string::npos);

  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  (void)dialog.widget_data();                                  // populate claudeModelCombo, like a real Settings open
  EXPECT_FALSE(dialog.onIndexChanged("claudeModelCombo", 2));  // "sonnet", the first listed model -- backend unchanged
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  const std::string text = entryIn(snapshot(dialog), "transcriptText").at("plain_text").get<std::string>();
  EXPECT_NE(text.find("Give me a demo"), std::string::npos) << "the earlier conversation must still be there";
  std::size_t resumed_count = 0;
  for (std::size_t pos = text.find("resumed previous conversation"); pos != std::string::npos;
       pos = text.find("resumed previous conversation", pos + 1)) {
    ++resumed_count;
  }
  EXPECT_EQ(resumed_count, 1u) << "the conversation must not have been reloaded a second time";
  EXPECT_NE(text.find("Settings saved (backend: claude)"), std::string::npos);
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "session_alpha");
  EXPECT_EQ(SettingsStore(settings_view_).getString("assistant.claude.model", "unset"), "sonnet")
      << "the model change itself must still have been persisted";
}

TEST_F(AssistantDialogSettingsSwitchTest, SettingsPrefillMapsThePersistedModelOntoTheCombo) {
  {
    SettingsStore store(settings_view_);
    store.setString("assistant.claude.model", "");              // -> CLI default
    store.setString("assistant.codex.model", "made-up-model");  // -> not in this build's catalog -> Custom
  }
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  const json snap = snapshot(dialog);

  const json claude_combo = entryIn(snap, "claudeModelCombo");
  ASSERT_TRUE(claude_combo.contains("items"));
  const std::vector<std::string> claude_items = claude_combo.at("items").get<std::vector<std::string>>();
  ASSERT_GE(claude_items.size(), 3u) << "CLI default, Custom..., plus at least one of Claude's curated models";
  EXPECT_EQ(claude_items[0], "CLI default");
  EXPECT_EQ(claude_items[1], "Custom...");
  EXPECT_EQ(claude_combo.value("current_index", -1), 0) << "an explicitly empty stored value maps to CLI default";
  EXPECT_EQ(entryIn(snap, "claudeModelEdit").value("text", std::string("not-empty")), "");

  const json codex_combo = entryIn(snap, "codexModelCombo");
  EXPECT_EQ(codex_combo.value("current_index", -1), 1)
      << "a stored id this build's catalog does not list must fall back to Custom";
  EXPECT_EQ(entryIn(snap, "codexModelEdit").value("text", std::string("")), "made-up-model");

  // A listed id maps onto its own row, in the order ClaudeBackend::availableModels() returns them.
  {
    SettingsStore store(settings_view_);
    store.setString("assistant.claude.model", "opus");
  }
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  const json snap2 = snapshot(dialog);
  const std::vector<std::string> ids_in_order = {"sonnet", "opus", "fable", "haiku"};
  const auto it = std::find(ids_in_order.begin(), ids_in_order.end(), "opus");
  ASSERT_NE(it, ids_in_order.end());
  const int expected_index = 2 + static_cast<int>(it - ids_in_order.begin());
  EXPECT_EQ(entryIn(snap2, "claudeModelCombo").value("current_index", -1), expected_index);
  EXPECT_EQ(entryIn(snap2, "claudeModelEdit").value("text", std::string("not-empty")), "");
}

TEST_F(AssistantDialogSettingsSwitchTest, AcceptingACustomModelPersistsTheTypedText) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  (void)dialog.widget_data();  // populate the combo before picking a row

  // Mirrors the host's harvest order (every QLineEdit's current text, THEN
  // every QComboBox's index -- see the "Host facts" note in the brief).
  EXPECT_FALSE(dialog.onTextChanged("claudeModelEdit", "claude-3-7-unreleased"));
  EXPECT_FALSE(dialog.onIndexChanged("claudeModelCombo", 1));  // Custom...
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  EXPECT_EQ(SettingsStore(settings_view_).getString("assistant.claude.model", "unset"), "claude-3-7-unreleased");
}

TEST_F(AssistantDialogSettingsSwitchTest, PickingCustomWithAnUntouchedTextBoxPersistsEmpty) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  {
    SettingsStore store(settings_view_);
    store.setString("assistant.claude.model", "opus");  // something non-empty already persisted
  }
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  (void)dialog.widget_data();
  // The host still harvests the (hidden, never edited) text box; its shown
  // value is empty (opus is a listed model, so the prefill put it on the
  // combo row, not the text box) -- picking Custom without typing anything
  // must persist that empty string, not silently keep "opus".
  EXPECT_FALSE(dialog.onTextChanged("claudeModelEdit", ""));
  EXPECT_FALSE(dialog.onIndexChanged("claudeModelCombo", 1));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  EXPECT_EQ(SettingsStore(settings_view_).getString("assistant.claude.model", "unset"), "");
}

}  // namespace
