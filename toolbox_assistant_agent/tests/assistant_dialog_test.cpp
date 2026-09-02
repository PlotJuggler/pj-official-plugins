// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The conversations drawer end to end, against a REAL ClaudeBackend pointed
// at a throwaway $HOME: two of tests/fixtures/sessions/'s files are copied
// into the harness store that ClaudeBackend would read from
// (~/.claude/projects/<slug>/), so listConversations/loadTranscript/
// deleteConversation exercise the real claude_sessions.hpp parser, not a
// mock. None of the paths this file drives (menuButton, conversationList's
// selection/delete) ever call sendUserMessage or testConnection, so no
// `claude` CLI is spawned — this stays fully offline.
#include "assistant_dialog.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>  // tzset
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/settings_store_host.hpp>
#include <string>
#include <vector>

#include "claude_sessions.hpp"
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

TEST_F(AssistantDialogDrawerTest, OpeningTheDrawerListsRealHarnessConversationsNewestFirst) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);

  ASSERT_TRUE(dialog.onClicked("menuButton"));
  const json snap = snapshot(dialog);
  const json list = entryIn(snap, "conversationList");

  ASSERT_TRUE(list.contains("list_items"));
  const std::vector<std::string> items = list.at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 2u);
  // session_gamma (no ai-title -> falls back to the first prompt, truncated)
  // is newer than session_alpha (ai-title "PlotJuggler demo") -- newest first.
  EXPECT_EQ(items[0], "Can you plot the vehicle speed against the steer · 2 Sep 09:00");
  EXPECT_EQ(items[1], "PlotJuggler demo · 1 Sep 15:17");

  EXPECT_EQ(entryIn(snap, "conversationsDrawer").value("visible", false), true);
}

TEST_F(AssistantDialogDrawerTest, SelectingAConversationReplaysItsTranscriptAndPersistsTheActiveId) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_TRUE(dialog.onClicked("menuButton"));
  (void)dialog.widget_data();  // flush the list-open render before selecting

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

  EXPECT_EQ(entryIn(snap, "conversationsDrawer").value("visible", false), true)
      << "picking a conversation leaves the drawer open, like a sidebar";

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_)), "session_gamma");
}

TEST_F(AssistantDialogDrawerTest, DeletingTheActiveConversationStartsANewOneAndRemovesTheFile) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_TRUE(dialog.onClicked("menuButton"));
  ASSERT_TRUE(dialog.onSelectionChanged(
      "conversationList", {"Can you plot the vehicle speed against the steer · 2 Sep 09:00"}));
  (void)dialog.widget_data();

  ASSERT_TRUE(dialog.onClicked("menuButton"));  // reopen: refreshes the list again
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

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_)), "");

  const std::filesystem::path gamma_file = claudeSessionsDir(expectedWorkDir(home_).string()) / "session_gamma.jsonl";
  EXPECT_FALSE(std::filesystem::exists(gamma_file)) << "the file itself must be gone, not just delisted";
}

TEST_F(AssistantDialogDrawerTest, ReopeningThePanelResumesThePersistedConversation) {
  // What a previous instance leaves behind: only the active id. The transcript
  // itself has to come back from the harness's store on the first bind.
  {
    SettingsStore store(settings_view_);
    saveActiveSessionId(store, "session_alpha");
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
  EXPECT_EQ(entryIn(snap, "conversationsDrawer").value("visible", true), false) << "reopening keeps the drawer shut";
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_)), "session_alpha");
}

TEST_F(AssistantDialogDrawerTest, APurgedActiveConversationIsForgottenOnReopen) {
  {
    SettingsStore store(settings_view_);
    saveActiveSessionId(store, "session_vanished");  // Claude's retention took the file
  }
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  const json snap = snapshot(dialog);

  EXPECT_EQ(entryIn(snap, "transcriptText").value("plain_text", std::string("not-empty")), "")
      << "nothing to replay, so the panel opens blank rather than half-resumed";
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_)), "")
      << "a dangling --resume target must not survive to the next turn";
}

}  // namespace
