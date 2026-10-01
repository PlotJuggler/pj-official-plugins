// SPDX-License-Identifier: MPL-2.0
// Compile the actual editor implementation so its tick continuation and teardown
// are exercised without a Qt dependency or a fake replacement state machine.
#include <gtest/gtest.h>

#include "../../toolbox_assistant_agent/tests/support/fake_catalog_host.hpp"
#include "../../toolbox_assistant_agent/tests/support/recording_dp_host.hpp"
#include "../transform_editor_plugin.cpp"

namespace {

// What a trial of `return {cropped = ..., count = ...}` reports: the outputs it inferred and one bundle.
const char* const kTrialReport =
    R"({"coverage":{"complete":true},"bundles":[{"requested_ns":0,"stamp_ns":0,"inputs":[],"outputs":{)"
    R"("cropped":{"status":"ok","summary":{"type":"kPointCloud","points":23144}},"count":{"status":"ok","value":42}}}],)"
    R"("outputs":[{"name":"cropped","type":"kPointCloud"},{"name":"count","type":"number"}]})";

class TransformEditorPreviewTestPeer {
 public:
  static void bind(TransformEditorToolbox& editor, PJ::sdk::DataProcessorsHostView host) {
    editor.dp_view_ = host;
    editor.edit_debounce_ = std::chrono::milliseconds(0);  // no waiting for quiet in a test
  }
  // The form resolves inputs against the catalog (object topics first), so a config-driven
  // preview needs one that knows "/cloud".
  static void bindCatalog(TransformEditorToolbox& editor, assistant_agent::testing::FakeCatalogHost& catalog) {
    editor.test_catalog_host_ = PJ::sdk::ToolboxHostView(catalog.makeHost());
  }
  static void tick(TransformEditorToolbox& editor, std::string script = "return {count=1}") {
    PJ::sdk::DataProcessorRequest request;
    request.kind = "on_demand";
    request.language = "luau";
    request.inputs = {"/cloud"};
    request.script = script;
    request.params_json = "{}";
    // The request is injected past the form, so tell the editor the form moved (its caches are per revision).
    editor.dialog_.catalogChanged();
    TransformEditorToolbox::OnDemandBuild build;
    build.request = request;
    TransformEditorToolbox::setTrialForm(build);
    editor.previewOnDemand(build);
  }
  static void setDebounce(TransformEditorToolbox& editor, std::chrono::milliseconds value) {
    editor.edit_debounce_ = value;
  }
  static bool pending(const TransformEditorToolbox& editor) {
    return editor.pending_preview_.has_value();
  }
  static void refresh(TransformEditorToolbox& editor) {
    editor.next_preview_refresh_ = std::chrono::steady_clock::time_point{};
  }
  static void expire(TransformEditorToolbox& editor) {
    editor.preview_deadline_ = std::chrono::steady_clock::time_point{};
  }
  static void configure(TransformEditorToolbox& editor, bool source, bool body) {
    nlohmann::json config = {
        {"global_code", "-- pj-kind: on_demand\n-- pj-outputs: count:number"},
        {"function_body", body ? "return {count=1}" : ""},
        {"sources", source ? nlohmann::json::array({"/cloud"}) : nlohmann::json::array()}};
    ASSERT_TRUE(editor.dialog_.loadConfig(config.dump()));
  }
  static void refreshCurrent(TransformEditorToolbox& editor) {
    editor.refreshPreview();
  }
  static std::string status(TransformEditorToolbox& editor) {
    return nlohmann::json::parse(editor.dialog_.widget_data())["statusLabel"]["label"];
  }
  // The status line shows the reason Create is disabled once no result is left, so "cleared" means that neither
  // the result of the trial nor a pending run is on screen any more.
  static bool showsReport(TransformEditorToolbox& editor) {
    return status(editor).find("count: 42") != std::string::npos;
  }
  static void close(TransformEditorToolbox& editor) {
    editor.tearDownPreview();
  }
};
using Host = assistant_agent::testing::RecordingDpHost;
TEST(TransformEditorPreview, PendingYieldsThenCompletesAndReleasesOnce) {
  Host host;
  host.pending_polls = 2;
  host.canned_report_json = kTrialReport;
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host.view());
  TransformEditorPreviewTestPeer::tick(editor);
  EXPECT_EQ(host.poll_calls, 1);
  EXPECT_TRUE(TransformEditorPreviewTestPeer::pending(editor));
  EXPECT_TRUE(host.released_handles.empty());
  TransformEditorPreviewTestPeer::tick(editor);
  TransformEditorPreviewTestPeer::tick(editor);
  EXPECT_FALSE(TransformEditorPreviewTestPeer::pending(editor));
  EXPECT_EQ(host.submit_calls, 1);
  EXPECT_EQ(host.released_handles.size(), 1u);
  TransformEditorPreviewTestPeer::close(editor);
  EXPECT_EQ(host.released_handles.size(), 1u);
}
TEST(TransformEditorPreview, CompletedSameInstantRefreshesForLiveInputChanges) {
  Host host;
  host.canned_report_json = kTrialReport;
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host.view());
  TransformEditorPreviewTestPeer::tick(editor);
  TransformEditorPreviewTestPeer::tick(editor);
  EXPECT_EQ(host.submit_calls, 1);
  TransformEditorPreviewTestPeer::refresh(editor);
  TransformEditorPreviewTestPeer::tick(editor);
  EXPECT_EQ(host.submit_calls, 2);
  EXPECT_EQ(host.released_handles.size(), 2u);
}
TEST(TransformEditorPreview, FailedAndCancelledReleaseOnce) {
  for (auto state : {PJ_EVALUATION_STATE_FAILED, PJ_EVALUATION_STATE_CANCELLED}) {
    Host host;
    host.pending_polls = 1;
    host.terminal_state = state;
    host.canned_report_json = R"({"error":"deliberate failure"})";
    TransformEditorToolbox editor;
    TransformEditorPreviewTestPeer::bind(editor, host.view());
    TransformEditorPreviewTestPeer::tick(editor);
    TransformEditorPreviewTestPeer::tick(editor);
    EXPECT_FALSE(TransformEditorPreviewTestPeer::pending(editor));
    EXPECT_EQ(host.released_handles.size(), 1u);
  }
}
TEST(TransformEditorPreview, ExpiryReleasesOnceWithoutBlocking) {
  Host host;
  host.pending_polls = 100;
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host.view());
  TransformEditorPreviewTestPeer::tick(editor);
  TransformEditorPreviewTestPeer::expire(editor);
  TransformEditorPreviewTestPeer::tick(editor);
  EXPECT_FALSE(TransformEditorPreviewTestPeer::pending(editor));
  EXPECT_EQ(host.released_handles.size(), 1u);
}
TEST(TransformEditorPreview, ReplacementAndDestructionReleasePendingRequests) {
  Host host;
  host.pending_polls = 100;
  {
    TransformEditorToolbox editor;
    TransformEditorPreviewTestPeer::bind(editor, host.view());
    TransformEditorPreviewTestPeer::tick(editor);
    TransformEditorPreviewTestPeer::tick(editor, "return {count=2}");
    EXPECT_EQ(host.submit_calls, 2);
    EXPECT_EQ(host.released_handles.size(), 1u);
  }
  EXPECT_EQ(host.released_handles.size(), 2u);
}
TEST(TransformEditorPreview, AnEditWaitsForTheDebounceAndDropsTheRunInFlight) {
  Host host;
  host.pending_polls = 100;
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host.view());
  TransformEditorPreviewTestPeer::tick(editor);
  ASSERT_TRUE(TransformEditorPreviewTestPeer::pending(editor));
  TransformEditorPreviewTestPeer::setDebounce(editor, std::chrono::seconds(30));  // the user keeps typing
  TransformEditorPreviewTestPeer::tick(editor, "return {count=2}");
  EXPECT_FALSE(TransformEditorPreviewTestPeer::pending(editor)) << "the stale run is released at once";
  EXPECT_EQ(host.released_handles.size(), 1u);
  EXPECT_EQ(host.submit_calls, 1) << "nothing is submitted while the edits keep coming";
  TransformEditorPreviewTestPeer::setDebounce(editor, std::chrono::milliseconds(0));
  TransformEditorPreviewTestPeer::tick(editor, "return {count=2}");
  EXPECT_EQ(host.submit_calls, 2);
}

}  // namespace

TEST(TransformEditorPreview, TheStatusLineShowsTheResultAndCreateWaitsForAnInferredTrial) {
  TransformEditorDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(
      R"({"global_code":"-- pj-kind: on_demand","function_body":"return {count=1}","sources":["/cloud"]})"));
  dialog.setStatus("count: 42");
  const auto widgets = nlohmann::json::parse(dialog.widget_data());
  EXPECT_EQ(widgets["statusLabel"]["label"], "count: 42");
  EXPECT_EQ(widgets["pushButtonCreate"]["enabled"], false) << "no trial has succeeded yet";
}

TEST(TransformEditorPreview, ClearingBodyOrInputClearsCompletedAndPendingReports) {
  for (bool clear_source : {false, true}) {
    Host host;
    host.canned_report_json = kTrialReport;
    assistant_agent::testing::FakeCatalogHost catalog;
    catalog.addObjectTopic("/cloud", "kPointCloud", 1, 0, 1);
    TransformEditorToolbox editor;
    TransformEditorPreviewTestPeer::bind(editor, host.view());
    TransformEditorPreviewTestPeer::bindCatalog(editor, catalog);
    TransformEditorPreviewTestPeer::configure(editor, true, true);
    TransformEditorPreviewTestPeer::refreshCurrent(editor);
    ASSERT_TRUE(TransformEditorPreviewTestPeer::showsReport(editor));
    TransformEditorPreviewTestPeer::configure(editor, !clear_source, clear_source);
    TransformEditorPreviewTestPeer::refreshCurrent(editor);
    EXPECT_FALSE(TransformEditorPreviewTestPeer::showsReport(editor));

    host.pending_polls = 100;
    TransformEditorPreviewTestPeer::configure(editor, true, true);
    TransformEditorPreviewTestPeer::refreshCurrent(editor);
    ASSERT_TRUE(TransformEditorPreviewTestPeer::pending(editor));
    const int polls = host.poll_calls;
    TransformEditorPreviewTestPeer::configure(editor, !clear_source, clear_source);
    TransformEditorPreviewTestPeer::refreshCurrent(editor);
    EXPECT_FALSE(TransformEditorPreviewTestPeer::pending(editor));
    EXPECT_EQ(host.released_handles.size(), 2u);
    // A terminal report arriving after invalidation must never be polled back
    // into the editor: the old handle was released and the recipe is incomplete.
    host.pending_polls = 0;
    TransformEditorPreviewTestPeer::refreshCurrent(editor);
    EXPECT_EQ(host.poll_calls, polls);
    EXPECT_FALSE(TransformEditorPreviewTestPeer::showsReport(editor));
  }
}
