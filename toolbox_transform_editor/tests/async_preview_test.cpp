// SPDX-License-Identifier: MPL-2.0
// Compile the actual editor implementation so its tick continuation and teardown
// are exercised without a Qt dependency or a fake replacement state machine.
#include <gtest/gtest.h>

#include "../../toolbox_assistant_agent/tests/support/fake_catalog_host.hpp"
#include "../../toolbox_assistant_agent/tests/support/recording_dp_host.hpp"
#include "../transform_editor_plugin.cpp"

namespace {
class TransformEditorPreviewTestPeer {
 public:
  static void bind(TransformEditorToolbox& editor, PJ::sdk::DataProcessorsHostView host) {
    editor.dp_view_ = host;
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
    request.outputs = {{"count", "number"}};
    request.script = script;
    request.params_json = "{}";
    editor.previewOnDemand(request);
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
  static std::string report(TransformEditorToolbox& editor) {
    return nlohmann::json::parse(editor.dialog_.widget_data())["onDemandReportPreview"]["plain_text"];
  }
  // The pane shows the form's own hints once no report is left, so "cleared" means that neither
  // the completed report nor the pending placeholder is on screen any more.
  static bool showsReport(TransformEditorToolbox& editor) {
    const std::string text = report(editor);
    return text.find("count: 42") != std::string::npos || text.find("Evaluating") != std::string::npos;
  }
  static void close(TransformEditorToolbox& editor) {
    editor.tearDownPreview();
  }
};
using Host = assistant_agent::testing::RecordingDpHost;
TEST(TransformEditorPreview, PendingYieldsThenCompletesAndReleasesOnce) {
  Host host;
  host.pending_polls = 2;
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
}  // namespace

TEST(TransformEditorPreview, ReportIsVisibleInMainPanelAndScalarCreateIsDisabled) {
  TransformEditorDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(
      R"({"global_code":"-- pj-kind: on_demand","function_body":"return {count=1}","sources":["/cloud"]})"));
  dialog.setOnDemandReport("{\"count\":42}");
  const auto widgets = nlohmann::json::parse(dialog.widget_data());
  EXPECT_NE(dialog.ui_content().find("name=\"onDemandReportPreview\""), std::string::npos);
  EXPECT_EQ(widgets["onDemandReportPreview"]["visible"], true);
  EXPECT_EQ(widgets["onDemandReportPreview"]["plain_text"], "{\"count\":42}");
  EXPECT_EQ(widgets["framePlotPreview"]["visible"], false);
  EXPECT_EQ(widgets["pushButtonCreate"]["enabled"], false);
}

TEST(TransformEditorPreview, ClearingBodyOrInputClearsCompletedAndPendingReports) {
  for (bool clear_source : {false, true}) {
    Host host;
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
