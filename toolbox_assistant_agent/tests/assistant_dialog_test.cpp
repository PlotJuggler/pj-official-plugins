// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The conversations drawer end to end, against a REAL ClaudeBackend pointed at a throwaway $HOME:
// two of tests/fixtures/sessions/'s files are copied into the store ClaudeBackend reads
// (~/.claude/projects/<slug>/), so the real claude_sessions.hpp parser is exercised. Turns point the
// CLI at a name that does not resolve, or fail earlier for want of an MCP endpoint, so no `claude`
// CLI is spawned. The drawer refreshes on bind, on a backend switch, after a delete and after every
// completed turn.
#include "assistant_dialog.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/feature_catalog_host.hpp>
#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/settings_store_host.hpp>
#include <pj_base/sdk/testing/fake_mcp_endpoint_host.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>
#include <thread>
#include <vector>

#include "catalog_digest.hpp"
#include "claude_sessions.hpp"
#include "codex_sessions.hpp"
#include "conversation_state.hpp"
#include "fallback_features.hpp"
#include "features_digest.hpp"
#include "harness_memory.hpp"
#include "settings_store.hpp"
#include "test_support/scoped_env.hpp"

#ifndef ASSISTANT_SESSIONS_FIXTURES_DIR
#error "ASSISTANT_SESSIONS_FIXTURES_DIR must be defined by CMake"
#endif

namespace assistant_agent {

class AssistantDialogTestPeer {
 public:
  static void setCompletedSession(AssistantDialog& dialog, const std::string& id) {
    dialog.memoryFor(dialog.active_backend_key_)->session_id = id;
    dialog.applyBackendEvent({BackendEvent::Kind::TurnComplete, {}});
  }

  static void finishWithError(AssistantDialog& dialog, const std::string& error) {
    dialog.applyBackendEvent({BackendEvent::Kind::Error, error});
    dialog.applyBackendEvent({BackendEvent::Kind::TurnComplete, {}});
  }

  static std::string featuresForTurn(AssistantDialog& dialog) {
    return dialog.featuresForTurn();
  }

  // Sends `text` as the user would and pumps onTick() until the turn is over; false if it is still
  // running after a few seconds (a hang).
  static bool runTurn(AssistantDialog& dialog, const std::string& text) {
    dialog.onTextChanged("inputEdit", text);
    if (!dialog.onClicked("sendButton")) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      dialog.onTick();
      {
        std::lock_guard<std::mutex> lock(dialog.state_.mu);
        if (!dialog.state_.session.busy()) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  static std::string transcript(AssistantDialog& dialog) {
    std::lock_guard<std::mutex> lock(dialog.state_.mu);
    std::string out;
    for (const auto& m : dialog.state_.session.messages()) {
      out += m.text + "\n";
    }
    return out;
  }
};

}  // namespace assistant_agent

namespace {

using assistant_agent::AssistantDialog;
using assistant_agent::AssistantDialogTestPeer;
using assistant_agent::catalogDigest;
using assistant_agent::claudeSessionsDir;
using assistant_agent::loadActiveSessionId;
using assistant_agent::loadConversationTitles;
using assistant_agent::resolveCatalogBudgetChars;
using assistant_agent::resolveFeaturesBudgetChars;
using assistant_agent::SettingsStore;
using nlohmann::json;

using toolbox_testing::makeTempDir;
using toolbox_testing::refreshTimezone;
using toolbox_testing::ScopedEnv;

TEST(AssistantDialogUi, TranscriptOptsIntoMarkdownWithoutRequiringANewWidgetClass) {
  AssistantDialog dialog;
  const std::string ui = dialog.ui_content();
  EXPECT_NE(ui.find(R"(<widget class="QPlainTextEdit" name="transcriptText">)"), std::string::npos);
  EXPECT_NE(ui.find(R"(<property name="pjMarkdown" stdset="0"><bool>true</bool></property>)"), std::string::npos);
  EXPECT_NE(ui.find(R"(<property name="pjFollowTail" stdset="0"><bool>true</bool></property>)"), std::string::npos);
}

// The notice is its own row, not inside headerBand (the host hides headerBand), and carries the link.
TEST(AssistantDialogUi, DeprecationNoticeIsVisibleOutsideTheHeaderBandAndLinksToTheSetupGuide) {
  AssistantDialog dialog;
  const std::string ui = dialog.ui_content();
  const size_t notice = ui.find(R"(<widget class="QLabel" name="deprecationNotice">)");
  const size_t body = ui.find(R"(name="bodyRow")");
  const size_t header = ui.rfind("<widget", ui.find(R"(name="headerBand")"));
  ASSERT_NE(notice, std::string::npos);
  ASSERT_NE(body, std::string::npos);
  ASSERT_NE(header, std::string::npos);
  ASSERT_LT(header, notice);
  ASSERT_LT(notice, body);
  // Every widget opened since headerBand is closed before the notice: it is not in the band.
  auto count = [&](const std::string& needle) {
    size_t n = 0;
    for (size_t pos = ui.find(needle, header); pos != std::string::npos && pos < notice;
         pos = ui.find(needle, pos + 1)) {
      ++n;
    }
    return n;
  };
  EXPECT_EQ(count("<widget "), count("</widget>"));

  const std::string label = ui.substr(notice, body - notice);
  EXPECT_NE(
      label.find("We suggest using PlotJuggler from Claude Code or Codex through its MCP server instead."),
      std::string::npos);
  EXPECT_NE(label.find("https://github.com/PlotJuggler/plotjuggler-agent#install"), std::string::npos);
  EXPECT_NE(label.find(R"(<property name="openExternalLinks"><bool>true</bool></property>)"), std::string::npos);
  EXPECT_NE(label.find(R"(<property name="wordWrap"><bool>true</bool></property>)"), std::string::npos);
  EXPECT_EQ(label.find(R"(name="visible")"), std::string::npos);
}

// Mirrors ensureWorkDir's resolution (harness_workdir.cpp): the SDK's per-user data dir plus the
// fixed "pj-assistant-cli" leaf. The fixtures below redirect userDataDir() into a temp dir.
std::filesystem::path expectedWorkDir() {
  return PJ::sdk::userDataDir() / "pj-assistant-cli";
}

// Proves the redirection took: without it these tests would still pass (they seed and read the same
// path production computes) while creating a directory in the real home. Called by every fixture
// right after its ScopedEnvs.
::testing::AssertionResult workDirIsInside(const std::filesystem::path& home) {
  const std::string work = expectedWorkDir().string();
  const std::string root = home.string();
  if (work.rfind(root, 0) == 0) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure() << "the work dir resolved to " << work << ", which is outside the test's "
                                       << root << " -- PJ::sdk::userDataDir() was not redirected";
}

// A fresh $HOME per test with two fixtures seeded into the directory ClaudeBackend reads.
class AssistantDialogDrawerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    home_ = makeTempDir("assistant_dialog_test_");
    ASSERT_FALSE(home_.empty());
    home_env_ = std::make_unique<ScopedEnv>("HOME", home_);
    // PJ::sdk::userDataDir() reads XDG_DATA_HOME then HOME on Linux, HOME on macOS, LOCALAPPDATA on
    // Windows; clearing the first and pointing the others at the temp dir puts the work dir inside it.
    xdg_env_ = std::make_unique<ScopedEnv>("XDG_DATA_HOME", nullptr);
    localappdata_env_ = std::make_unique<ScopedEnv>("LOCALAPPDATA", home_);
    cfg_env_ = std::make_unique<ScopedEnv>("CLAUDE_CONFIG_DIR", nullptr);
    // Rows carry LOCAL time; pin the zone so the asserted row texts do not move with the machine.
    tz_env_ = std::make_unique<ScopedEnv>("TZ", "UTC");
    refreshTimezone();
    ASSERT_TRUE(workDirIsInside(home_));

    std::error_code ec;

    const std::filesystem::path sessions_dir = claudeSessionsDir(expectedWorkDir().string());
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

  // widget_data() drains its dirty flags on every call, so a checkpoint inspecting several widgets
  // must snapshot ONCE and read every field from that document: a second call with nothing newly
  // dirty returns "{}", not the prior state.
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
  std::unique_ptr<ScopedEnv> localappdata_env_;
  std::unique_ptr<ScopedEnv> cfg_env_;
  std::unique_ptr<ScopedEnv> tz_env_;
  PJ::sdk::InMemorySettingsBackend backend_;
  PJ::sdk::SettingsStoreHost host_{backend_};
  PJ::sdk::SettingsView settings_view_;
};

TEST_F(AssistantDialogDrawerTest, ThePanelListsRealHarnessConversationsFromTheFirstFrame) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);  // the drawer is always visible: no open click needed

  const json snap = snapshot(dialog);
  const json list = entryIn(snap, "conversationList");

  ASSERT_TRUE(list.contains("list_items"));
  const std::vector<std::string> items = list.at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 2u);
  // session_gamma (no ai-title: first prompt, truncated) is newer than session_alpha (ai-title
  // "PlotJuggler demo").
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
  EXPECT_NE(text.find("**You:**\n\nCan you plot the vehicle speed against the steering angle?"), std::string::npos);
  EXPECT_NE(text.find(R"(list\_topics)"), std::string::npos)
      << "the escaped tool_use row, name only, like the live path";
  EXPECT_NE(
      text.find("**Assistant:**\n\nI opened a new tab plotting vehicle_speed against vehicle_steering."),
      std::string::npos);
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
  // ONE snapshot: the delete's list refresh and its startNewConversation() land in the same
  // widget_data() build; a second call would see nothing newly dirty.
  const json snap = snapshot(dialog);

  const std::vector<std::string> items_after =
      entryIn(snap, "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items_after.size(), 1u);
  EXPECT_EQ(items_after[0], "PlotJuggler demo · 1 Sep 15:17");

  EXPECT_EQ(entryIn(snap, "transcriptText").value("plain_text", std::string("not-empty")), "")
      << "deleting the active conversation starts a blank new one";

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "");

  const std::filesystem::path gamma_file = claudeSessionsDir(expectedWorkDir().string()) / "session_gamma.jsonl";
  EXPECT_FALSE(std::filesystem::exists(gamma_file)) << "the file itself must be gone, not just delisted";
}

TEST_F(AssistantDialogDrawerTest, ReopeningThePanelResumesThePersistedConversation) {
  // A previous instance leaves only the active id; the transcript must come back from the harness's
  // store on the first bind.
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

TEST_F(AssistantDialogDrawerTest, EveryCompletionRefreshesAndSelectsTheNewConversationWithoutLosingNames) {
  SettingsStore store(settings_view_);
  assistant_agent::setConversationTitle(store, "claude", "session_gamma", "Steering demo");

  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  (void)snapshot(dialog);
  ASSERT_TRUE(dialog.onClicked("newChatButton"));
  (void)snapshot(dialog);

  const std::filesystem::path sessions_dir = claudeSessionsDir(expectedWorkDir().string());
  std::ifstream source(sessions_dir / "session_alpha.jsonl");
  ASSERT_TRUE(source.is_open());
  std::string contents((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
  const auto replaceAll = [&contents](const std::string& from, const std::string& to) {
    for (std::size_t at = contents.find(from); at != std::string::npos; at = contents.find(from, at + to.size())) {
      contents.replace(at, from.size(), to);
    }
  };
  replaceAll("PlotJuggler demo", "Fresh conversation");
  replaceAll("2026-09-01", "2026-09-03");
  std::ofstream fresh(sessions_dir / "session_new.jsonl");
  ASSERT_TRUE(fresh.is_open());
  fresh << contents;
  fresh.close();

  AssistantDialogTestPeer::setCompletedSession(dialog, "session_new");
  const json completed = snapshot(dialog);
  const json completed_list = entryIn(completed, "conversationList");
  const std::vector<std::string> completed_items = completed_list.at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(completed_items.size(), 3u);
  EXPECT_EQ(std::count(completed_items.begin(), completed_items.end(), "Fresh conversation · 3 Sep 15:17"), 1);
  EXPECT_NE(
      std::find(completed_items.begin(), completed_items.end(), "Steering demo · 2 Sep 09:00"), completed_items.end());
  EXPECT_EQ(
      completed_list.at("selected_items").get<std::vector<std::string>>(),
      std::vector<std::string>({"Fresh conversation · 3 Sep 15:17"}));
  EXPECT_EQ(loadActiveSessionId(store, "claude"), "session_new");

  AssistantDialogTestPeer::setCompletedSession(dialog, "session_new");
  const std::vector<std::string> repeated =
      entryIn(snapshot(dialog), "conversationList").at("list_items").get<std::vector<std::string>>();
  EXPECT_EQ(repeated.size(), 3u);
  EXPECT_EQ(std::count(repeated.begin(), repeated.end(), "Fresh conversation · 3 Sep 15:17"), 1);

  for (const char* error : {"backend failed", "cancelled"}) {
    AssistantDialogTestPeer::finishWithError(dialog, error);
    const json after_error = entryIn(snapshot(dialog), "conversationList");
    EXPECT_EQ(after_error.at("list_items").get<std::vector<std::string>>().size(), 3u);
    EXPECT_EQ(
        after_error.at("selected_items").get<std::vector<std::string>>(),
        std::vector<std::string>({"Fresh conversation · 3 Sep 15:17"}));
    EXPECT_EQ(loadConversationTitles(store, "claude").at("session_gamma"), "Steering demo");
  }
}

TEST_F(AssistantDialogDrawerTest, RenamingTheActiveRowKeepsItActiveAndLeavesTheTranscriptAlone) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  ASSERT_TRUE(dialog.onSelectionChanged(
      "conversationList", {"Can you plot the vehicle speed against the steer · 2 Sep 09:00"}));
  (void)dialog.widget_data();  // flush the selection's render before renaming

  // Row 0 is still session_gamma: selecting does not reorder the list.
  ASSERT_TRUE(dialog.onItemContextAction("conversationList", 0, "rename"));
  (void)dialog.widget_data();  // the render requests the sub-dialog (sets open_sub_dialog)
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
    (void)dialog.widget_data();  // the render requests the sub-dialog (sets open_sub_dialog)
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
  (void)dialog.widget_data();  // the render requests the sub-dialog (sets open_sub_dialog)
  EXPECT_FALSE(dialog.onTextChanged("renameEdit", "Steering demo"));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  ASSERT_EQ(
      entryIn(snapshot(dialog), "conversationList").at("list_items").get<std::vector<std::string>>()[0],
      "Steering demo · 2 Sep 09:00");

  ASSERT_TRUE(dialog.onItemContextAction("conversationList", 0, "rename"));
  (void)dialog.widget_data();                               // the render requests the sub-dialog (sets open_sub_dialog)
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
    (void)dialog.widget_data();  // the render requests the sub-dialog (sets open_sub_dialog)
    EXPECT_FALSE(dialog.onTextChanged("renameEdit", "Steering demo"));
    EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  }

  // The harness's own retention purges the file, not our delete path: the prune is driven by the
  // LISTING, not by onItemDeleteRequested.
  std::error_code ec;
  std::filesystem::remove(claudeSessionsDir(expectedWorkDir().string()) / "session_gamma.jsonl", ec);
  ASSERT_FALSE(ec);

  AssistantDialog dialog2;
  dialog2.setSettings(settings_view_);  // the bind's refresh prunes the now-dangling title
  const std::vector<std::string> items =
      entryIn(snapshot(dialog2), "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0], "PlotJuggler demo · 1 Sep 15:17");

  // Gone from the store, not merely unused, or a later conversation reusing the id would inherit the name.
  EXPECT_TRUE(loadConversationTitles(SettingsStore(settings_view_), "claude").empty());
}

TEST_F(AssistantDialogDrawerTest, RenameAndSettingsSubDialogsRouteIndependently) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);

  // Opening the rename dialog and accepting it must not write any settings key.
  ASSERT_TRUE(dialog.onItemContextAction("conversationList", 0, "rename"));
  (void)dialog.widget_data();  // the render requests the sub-dialog (sets open_sub_dialog)
  EXPECT_FALSE(dialog.onTextChanged("renameEdit", "Steering demo"));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  EXPECT_FALSE(SettingsStore(settings_view_).contains("assistant.backend"))
      << "renaming must never touch a settings key";

  // Opening settings and accepting still commits.
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 0));  // stays on claude
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  EXPECT_EQ(SettingsStore(settings_view_).getString("assistant.backend", "unset"), "claude");
}

// Codex drawer cases. codex_sessions.hpp filters by the `payload.cwd` FIELD inside each rollout file,
// not by directory (unlike Claude's per-cwd project directories), so these fixtures are written by
// the test with `cwd` set to wherever this test's throwaway $HOME resolves CodexBackend's work dir.

// Writes a minimal valid rollout: one session_meta record plus one user/assistant response_item
// pair, all listCodexConversations / loadCodexTranscript look at.
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
    home_env_ = std::make_unique<ScopedEnv>("HOME", home_);
    // PJ::sdk::userDataDir() reads XDG_DATA_HOME then HOME on Linux, HOME on macOS, LOCALAPPDATA on
    // Windows; clearing the first and pointing the others at the temp dir puts the work dir inside it.
    xdg_env_ = std::make_unique<ScopedEnv>("XDG_DATA_HOME", nullptr);
    localappdata_env_ = std::make_unique<ScopedEnv>("LOCALAPPDATA", home_);
    cfg_env_ = std::make_unique<ScopedEnv>("CLAUDE_CONFIG_DIR", nullptr);
    codex_home_ = makeTempDir("assistant_dialog_codex_home_");
    ASSERT_FALSE(codex_home_.empty());
    codex_home_env_ = std::make_unique<ScopedEnv>("CODEX_HOME", codex_home_);
    tz_env_ = std::make_unique<ScopedEnv>("TZ", "UTC");
    refreshTimezone();
    ASSERT_TRUE(workDirIsInside(home_));

    std::error_code ec;

    // CodexBackend's work dir resolves through the same harness_workdir.hpp rules (expectedWorkDir).
    const std::string work_dir = expectedWorkDir().string();
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
    // Codex is active from the first bind, so rebuildBackend() builds a CodexBackend (not the ctor's
    // default ClaudeBackend) before anything resumes a conversation through it.
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
  std::unique_ptr<ScopedEnv> localappdata_env_;
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
  EXPECT_NE(text.find("**You:**\n\nCan you plot the vehicle speed against the steering angle?"), std::string::npos);
  EXPECT_NE(
      text.find("**Assistant:**\n\nI opened a new tab plotting vehicle_speed against vehicle_steering."),
      std::string::npos);
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

TEST_F(AssistantDialogCodexDrawerTest, FirstCompletionRefreshesAndSelectsTheNewRolloutOnce) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  (void)snapshot(dialog);
  ASSERT_TRUE(dialog.onClicked("newChatButton"));
  (void)snapshot(dialog);

  const std::filesystem::path rollout =
      codex_home_ / "sessions" / "2026" / "09" / "03" / "rollout-2026-09-03T10-00-00-codex_new.jsonl";
  writeCodexFixture(
      rollout, "codex_new", expectedWorkDir().string(), "2026-09-03T10:00:00.000Z", "Inspect fresh data",
      "The fresh data is ready.", "2026-09-03T10:00:05.000Z");

  AssistantDialogTestPeer::setCompletedSession(dialog, "codex_new");
  const json first = entryIn(snapshot(dialog), "conversationList");
  const std::vector<std::string> items = first.at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(std::count(items.begin(), items.end(), "Inspect fresh data · 3 Sep 10:00"), 1);
  EXPECT_EQ(
      first.at("selected_items").get<std::vector<std::string>>(),
      std::vector<std::string>({"Inspect fresh data · 3 Sep 10:00"}));
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "codex_new");

  AssistantDialogTestPeer::setCompletedSession(dialog, "codex_new");
  const std::vector<std::string> repeated =
      entryIn(snapshot(dialog), "conversationList").at("list_items").get<std::vector<std::string>>();
  EXPECT_EQ(repeated.size(), 3u);
  EXPECT_EQ(std::count(repeated.begin(), repeated.end(), "Inspect fresh data · 3 Sep 10:00"), 1);
}

TEST(AssistantDialogBackendSwitch, ClaudeCodexClaudeKeepsBothSessionIds) {
  const std::filesystem::path home = makeTempDir("assistant_dialog_switch_test_");
  ASSERT_FALSE(home.empty());
  ScopedEnv home_env("HOME", home);
  // Puts userDataDir() inside `home` (see the fixtures above).
  ScopedEnv xdg_env("XDG_DATA_HOME", nullptr);
  ScopedEnv localappdata_env("LOCALAPPDATA", home);
  ScopedEnv cfg_env("CLAUDE_CONFIG_DIR", nullptr);
  const std::filesystem::path codex_home = makeTempDir("assistant_dialog_switch_codex_home_");
  ASSERT_FALSE(codex_home.empty());
  ScopedEnv codex_home_env("CODEX_HOME", codex_home);
  ScopedEnv tz_env("TZ", "UTC");
  refreshTimezone();
  ASSERT_TRUE(workDirIsInside(home));

  std::error_code ec;
  const std::string work_dir = expectedWorkDir().string();

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
    // commitSettings() finishes by probing the new backend with testConnection() (a real
    // `<cli> --version` subprocess). Point both CLI paths at names that cannot resolve so this
    // session-id test never spawns a real `claude` or `codex`.
    SettingsStore store(settings_view);
    store.setString("assistant.claude.cli_path", "assistant-agent-test-missing-claude-cli");
    store.setString("assistant.codex.cli_path", "assistant-agent-test-missing-codex-cli");
  }

  AssistantDialog dialog;
  dialog.setSettings(settings_view);  // starts on the default, "claude"

  ASSERT_TRUE(dialog.onSelectionChanged("conversationList", {"PlotJuggler demo · 1 Sep 15:17"}));
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view), "claude"), "session_alpha");

  // Switch to Codex through the real settings flow (settingsButton -> backendCombo ->
  // subDialogAccepted). activateBackendConversation() refreshes the listing unconditionally, so it
  // is already codex's by the selection below.
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 1));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  ASSERT_TRUE(dialog.onSelectionChanged(
      "conversationList", {"Can you plot the vehicle speed against the steer · 2 Sep 09:00"}));
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view), "codex"), "codex_gamma");
  // Claude's id survives the switch untouched.
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

// SetUp() prepares a Claude session under the fake CLAUDE_CONFIG_DIR and a Codex rollout under the
// fake CODEX_HOME but persists no ACTIVE id: each test persists exactly the ids it needs.

class AssistantDialogSettingsSwitchTest : public ::testing::Test {
 protected:
  void SetUp() override {
    home_ = makeTempDir("assistant_settings_switch_test_");
    ASSERT_FALSE(home_.empty());
    home_env_ = std::make_unique<ScopedEnv>("HOME", home_);
    // PJ::sdk::userDataDir() reads XDG_DATA_HOME then HOME on Linux, HOME on macOS, LOCALAPPDATA on
    // Windows; clearing the first and pointing the others at the temp dir puts the work dir inside it.
    xdg_env_ = std::make_unique<ScopedEnv>("XDG_DATA_HOME", nullptr);
    localappdata_env_ = std::make_unique<ScopedEnv>("LOCALAPPDATA", home_);
    cfg_env_ = std::make_unique<ScopedEnv>("CLAUDE_CONFIG_DIR", nullptr);
    codex_home_ = makeTempDir("assistant_settings_switch_codex_home_");
    ASSERT_FALSE(codex_home_.empty());
    codex_home_env_ = std::make_unique<ScopedEnv>("CODEX_HOME", codex_home_);
    tz_env_ = std::make_unique<ScopedEnv>("TZ", "UTC");
    refreshTimezone();
    ASSERT_TRUE(workDirIsInside(home_));

    std::error_code ec;
    const std::string work_dir = expectedWorkDir().string();

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
    // commitSettings() finishes by probing the new backend with testConnection() (a real
    // `<cli> --version` subprocess); point both CLI paths at names that cannot resolve so these
    // tests never spawn a real CLI.
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
  std::unique_ptr<ScopedEnv> localappdata_env_;
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
      return;  // "no update": steady state
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
  checkPayload(dialog.widget_data());  // initial render; the drawer is always visible, so it already names the list

  // A settings commit that does NOT switch backends: rebuildBackend() sets controls_dirty but nothing
  // calls activateBackendConversation(), so drawer_dirty stays false: the payload names
  // conversationList via setEnabled alone, the shape that would drop the flag.
  ASSERT_TRUE(dialog.onClicked("settingsButton"));
  EXPECT_FALSE(dialog.onIndexChanged("backendCombo", 0));  // stays on claude
  ASSERT_TRUE(dialog.onClicked("subDialogAccepted"));
  checkPayload(dialog.widget_data());

  // A controls-only refresh: sendCurrentInput() sets transcript_dirty + controls_dirty synchronously,
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
  const std::size_t saved_at = text.find(R"(Settings saved \(backend\: codex\))");
  ASSERT_NE(resumed_at, std::string::npos);
  ASSERT_NE(saved_at, std::string::npos);
  EXPECT_LT(resumed_at, saved_at) << "the note must land in the transcript it switched TO, not the one just left";

  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "codex_gamma");
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "session_alpha")
      << "switching must not touch claude's persisted id";

  // activateBackendConversation() refreshes the drawer unconditionally, so `snap` already carries
  // codex's listing.
  const std::vector<std::string> items =
      entryIn(snap, "conversationList").at("list_items").get<std::vector<std::string>>();
  ASSERT_EQ(items.size(), 1u) << "the drawer listing must be codex's now, not claude's";
  EXPECT_EQ(items[0], "Can you plot the vehicle speed against the steer · 2 Sep 09:00");

  // Back to Claude: nothing lost.
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
    // Nothing persisted for codex, deliberately.
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
  EXPECT_NE(text.find(R"(Settings saved \(backend\: codex\))"), std::string::npos);
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "codex"), "");

  // `snap` already carries codex's listing (the refresh on a backend switch is unconditional).
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
  EXPECT_FALSE(dialog.onIndexChanged("claudeModelCombo", 2));  // "sonnet", the first listed model; backend unchanged
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));

  const std::string text = entryIn(snapshot(dialog), "transcriptText").at("plain_text").get<std::string>();
  EXPECT_NE(text.find("Give me a demo"), std::string::npos) << "the earlier conversation must still be there";
  std::size_t resumed_count = 0;
  for (std::size_t pos = text.find("resumed previous conversation"); pos != std::string::npos;
       pos = text.find("resumed previous conversation", pos + 1)) {
    ++resumed_count;
  }
  EXPECT_EQ(resumed_count, 1u) << "the conversation must not have been reloaded a second time";
  EXPECT_NE(text.find(R"(Settings saved \(backend\: claude\))"), std::string::npos);
  EXPECT_EQ(loadActiveSessionId(SettingsStore(settings_view_), "claude"), "session_alpha");
  EXPECT_EQ(SettingsStore(settings_view_).getString("assistant.claude.model", "unset"), "sonnet")
      << "the model change itself must still have been persisted";
}

TEST_F(AssistantDialogSettingsSwitchTest, SettingsPrefillMapsThePersistedModelOntoTheCombo) {
  {
    SettingsStore store(settings_view_);
    store.setString("assistant.claude.model", "");              // CLI default
    store.setString("assistant.codex.model", "made-up-model");  // not in this build's catalog: Custom
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

  // A listed id maps onto its own row, in ClaudeBackend::availableModels() order.
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

  // Mirrors the host's harvest order: every QLineEdit's text, THEN every QComboBox's index.
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
  // The host still harvests the hidden, never-edited text box; it is empty (opus is a listed model, so
  // the prefill used the combo row), so picking Custom without typing must persist "" and not keep "opus".
  EXPECT_FALSE(dialog.onTextChanged("claudeModelEdit", ""));
  EXPECT_FALSE(dialog.onIndexChanged("claudeModelCombo", 1));
  EXPECT_TRUE(dialog.onClicked("subDialogAccepted"));
  EXPECT_EQ(SettingsStore(settings_view_).getString("assistant.claude.model", "unset"), "");
}

// resolveCatalogBudgetChars is driven directly against a SettingsStore, not through a dialog turn:
// the dialog only forwards its value into catalogDigest.
TEST(AssistantDialogCatalogBudget, DefaultsToTenThousandWhenKeyIsAbsent) {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};
  SettingsStore store{PJ::sdk::SettingsView{host.view()}};
  EXPECT_EQ(resolveCatalogBudgetChars(store), 10000);
}

TEST(AssistantDialogCatalogBudget, UsesTheStoredValueWithinRange) {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};
  SettingsStore store{PJ::sdk::SettingsView{host.view()}};
  store.setString("assistant.catalog_budget_chars", "1000");
  EXPECT_EQ(resolveCatalogBudgetChars(store), 1000);
}

TEST(AssistantDialogCatalogBudget, ClampsBelowTheFloor) {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};
  SettingsStore store{PJ::sdk::SettingsView{host.view()}};
  store.setString("assistant.catalog_budget_chars", "1");
  EXPECT_EQ(resolveCatalogBudgetChars(store), 1000);
}

TEST(AssistantDialogCatalogBudget, ClampsAboveTheCeiling) {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};
  SettingsStore store{PJ::sdk::SettingsView{host.view()}};
  store.setString("assistant.catalog_budget_chars", "999999999");
  EXPECT_EQ(resolveCatalogBudgetChars(store), 200000);
}

TEST(AssistantDialogCatalogBudget, AnUnparsableValueFallsBackToTheDefault) {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};
  SettingsStore store{PJ::sdk::SettingsView{host.view()}};
  store.setString("assistant.catalog_budget_chars", "not-a-number");
  EXPECT_EQ(resolveCatalogBudgetChars(store), 10000);
}

// A configured budget shrinks and truncates the digest; leaving the key untouched reproduces
// catalogDigest's own default byte for byte.
TEST(AssistantDialogCatalogBudget, FeedsCatalogDigestWithATighterBudget) {
  PJ::testing::ToolboxTestStore data_store;
  for (int i = 0; i < 400; ++i) {
    const std::string name = "/topic_with_a_fairly_long_name_" + std::to_string(i);
    data_store.addTopic(name);
    data_store.addField(name, "value", {0}, {1.0});
  }

  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};
  SettingsStore settings{PJ::sdk::SettingsView{host.view()}};
  settings.setString("assistant.catalog_budget_chars", "1000");

  // As CatalogDigest.SaysSoWhenTruncated: the body stays within budget_chars but the header/footer
  // sentences are added on top, so the total is only under a small multiple of the budget.
  const std::string digest =
      catalogDigest(PJ::sdk::ToolboxHostView(data_store.makeHost()), resolveCatalogBudgetChars(settings));
  EXPECT_LT(digest.size(), 2000u) << "the digest must respect its (settings-derived) budget: " << digest;
  EXPECT_NE(digest.find("TRUNCATED"), std::string::npos) << digest;
}

TEST(AssistantDialogCatalogBudget, AbsentKeyReproducesTheDefaultBudgetDigestByteForByte) {
  PJ::testing::ToolboxTestStore data_store;
  for (int i = 0; i < 400; ++i) {
    const std::string name = "/topic_with_a_fairly_long_name_" + std::to_string(i);
    data_store.addTopic(name);
    data_store.addField(name, "value", {0}, {1.0});
  }

  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};
  SettingsStore settings{PJ::sdk::SettingsView{host.view()}};  // key never set

  const std::string digest_via_settings =
      catalogDigest(PJ::sdk::ToolboxHostView(data_store.makeHost()), resolveCatalogBudgetChars(settings));
  const std::string digest_default_arg = catalogDigest(PJ::sdk::ToolboxHostView(data_store.makeHost()));
  EXPECT_EQ(digest_via_settings, digest_default_arg);
}

// A host that offers no pj.mcp_endpoint.v1 (registration failed) binds an empty view. A turn ends,
// not hangs, with a chat message saying why.
TEST_F(AssistantDialogSettingsSwitchTest, WithoutTheMcpEndpointServiceATurnEndsWithAMessageSayingWhy) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  dialog.setMcpEndpointHost(PJ::sdk::McpEndpointHostView{});
  ASSERT_TRUE(AssistantDialogTestPeer::runTurn(dialog, "hello"));
  const std::string transcript = AssistantDialogTestPeer::transcript(dialog);
  EXPECT_NE(transcript.find("Error: This PlotJuggler cannot serve the Assistant's tools"), std::string::npos)
      << transcript;
  EXPECT_NE(transcript.find("offers no MCP endpoint"), std::string::npos) << transcript;
}

// Every turn acquires the endpoint on the GUI thread and the host hands back the one it holds; a turn
// that ends on its own never releases it (the host does, on plugin destruction). Each turn ends at the
// CLI lookup since the name does not resolve.
TEST_F(AssistantDialogSettingsSwitchTest, EveryTurnAcquiresTheEndpointAndNoneReleasesIt) {
  PJ::sdk::testing::FakeMcpEndpointHost mcp;
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  dialog.setMcpEndpointHost(PJ::sdk::McpEndpointHostView(mcp.makeHost()));
  ASSERT_TRUE(AssistantDialogTestPeer::runTurn(dialog, "first"));
  ASSERT_TRUE(AssistantDialogTestPeer::runTurn(dialog, "second"));
  EXPECT_EQ(mcp.acquireCalls(), 2);
  EXPECT_EQ(mcp.releaseCalls(), 0);
  const std::string transcript = AssistantDialogTestPeer::transcript(dialog);
  EXPECT_EQ(transcript.find("cannot serve the Assistant's tools"), std::string::npos) << transcript;
  EXPECT_NE(transcript.find("assistant-agent-test-missing-claude-cli"), std::string::npos)
      << "both turns got past the endpoint to the CLI lookup: " << transcript;
}

// Stop releases the endpoint, so PlotJuggler revokes its token and stops what the turn left running;
// the next turn acquires a new one.
TEST_F(AssistantDialogSettingsSwitchTest, StopReleasesTheEndpointAndTheNextTurnAcquiresAgain) {
  PJ::sdk::testing::FakeMcpEndpointHost mcp;
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  dialog.setMcpEndpointHost(PJ::sdk::McpEndpointHostView(mcp.makeHost()));
  ASSERT_TRUE(AssistantDialogTestPeer::runTurn(dialog, "first"));
  ASSERT_EQ(mcp.releaseCalls(), 0);
  dialog.onClicked("cancelButton");
  EXPECT_EQ(mcp.releaseCalls(), 1);
  ASSERT_TRUE(AssistantDialogTestPeer::runTurn(dialog, "second"));
  EXPECT_EQ(mcp.acquireCalls(), 2);
  EXPECT_EQ(mcp.releaseCalls(), 1);
}

// A host without the endpoint service has nothing to give back: Stop still works.
TEST_F(AssistantDialogSettingsSwitchTest, StopWithoutTheMcpEndpointServiceDoesNothingMore) {
  AssistantDialog dialog;
  dialog.setSettings(settings_view_);
  dialog.setMcpEndpointHost(PJ::sdk::McpEndpointHostView{});
  EXPECT_FALSE(dialog.onClicked("cancelButton"));
}

// A host that omits pj.feature_catalog.v1: assistant_toolbox.cpp binds an empty FeatureCatalogView,
// which is what this provider returns. The turn must still carry the built-in features reference.
TEST(AssistantDialogFeatures, WithoutTheHostServiceEveryTurnCarriesTheBuiltInReference) {
  AssistantDialog dialog;
  dialog.setFeatureCatalogProvider([]() { return PJ::sdk::FeatureCatalogView{}; });
  const std::string features = AssistantDialogTestPeer::featuresForTurn(dialog);
  EXPECT_EQ(features, assistant_agent::fallbackFeatures());
  EXPECT_EQ(features.rfind("PlotJuggler features (PlotJuggler 4.1, built into this assistant):", 0), 0u);
}

TEST(AssistantDialogFeatures, WithTheHostServiceTheTurnCarriesTheHostListing) {
  const std::string doc =
      R"({"schema": 1, "host_version": "4.1.0", "core": [{"id": "layouts", "name": "Layouts", "summary": "Save it.",)"
      R"( "entry_point": "File → Save Layout..."}], "registry": {"state": "fetched"}})";
  PJ::sdk::FeatureCatalogHost host([&doc]() { return doc; });
  AssistantDialog dialog;
  dialog.setFeatureCatalogProvider([&host]() { return PJ::sdk::FeatureCatalogView(host.view()); });
  EXPECT_EQ(AssistantDialogTestPeer::featuresForTurn(dialog), assistant_agent::featuresDigest(doc));
}

TEST(AssistantDialogFeatures, NoProviderMeansNoFeaturesBlock) {
  AssistantDialog dialog;
  EXPECT_TRUE(AssistantDialogTestPeer::featuresForTurn(dialog).empty());
}

TEST(AssistantDialogFeaturesBudget, DefaultsAndClamps) {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};
  SettingsStore store{PJ::sdk::SettingsView{host.view()}};
  EXPECT_EQ(resolveFeaturesBudgetChars(store), 20000);
  store.setString("assistant.features_budget_chars", "5000");
  EXPECT_EQ(resolveFeaturesBudgetChars(store), 5000);
  store.setString("assistant.features_budget_chars", "10");
  EXPECT_EQ(resolveFeaturesBudgetChars(store), 5000);
  store.setString("assistant.features_budget_chars", "2000");
  EXPECT_EQ(resolveFeaturesBudgetChars(store), 5000) << "below the built-in reference's length";
  store.setString("assistant.features_budget_chars", "999999999");
  EXPECT_EQ(resolveFeaturesBudgetChars(store), 200000);
  store.setString("assistant.features_budget_chars", "lots");
  EXPECT_EQ(resolveFeaturesBudgetChars(store), 20000);
}

}  // namespace
