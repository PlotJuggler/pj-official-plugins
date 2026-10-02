// SPDX-License-Identifier: MPL-2.0
// The simple Transform Editor through the real toolbox and dialog (the plugin source is compiled in,
// like async_preview_test), against the recording and fake hosts the assistant's tests use: inputs, a
// script and one preview. Outputs are inferred by a trial run; the name is asked at Create. Nothing
// here runs a script: the recording host proves WHAT the editor asked the host to run or install.
#include <gtest/gtest.h>

#include <filesystem>

#include "../transform_editor_plugin.cpp"
#include "test_support/fake_catalog_host.hpp"
#include "test_support/fake_playback_viewport_hosts.hpp"
#include "test_support/fake_plot_tabs_host.hpp"
#include "test_support/recording_dp_host.hpp"

namespace {

using toolbox_testing::FakeCatalogHost;
using toolbox_testing::FakePlaybackHost;
using toolbox_testing::FakePlotTabsHost;
using toolbox_testing::RecordingDpHost;

class TransformEditorPreviewTestPeer {
 public:
  // The dialog host announces what it can do through set_host_info, as the real one does.
  static void announceHost(TransformEditorToolbox& editor, std::uint64_t capabilities) {
    PJ_dialog_host_info_t info{};
    info.struct_size = sizeof(info);
    info.capabilities = capabilities;
    PJ_error_t error{};
    const auto* vtable =
        PJ::DialogPluginBase::vtableWithCreate(static_cast<decltype(PJ_dialog_vtable_t::create)>(nullptr));
    PJ::DialogPluginBase* dialog = &editor.dialog_;
    ASSERT_TRUE(vtable->set_host_info(dialog, &info, &error)) << error.message;
  }
  static void bind(
      TransformEditorToolbox& editor, RecordingDpHost& dp, FakeCatalogHost& catalog, FakePlaybackHost& playback,
      FakePlotTabsHost& tabs, bool embeds_scene_views = true) {
    announceHost(editor, embeds_scene_views ? PJ_DIALOG_HOST_EMBEDS_SCENE_VIEWS : 0);
    editor.dp_view_ = dp.view();
    editor.test_catalog_host_ = PJ::sdk::ToolboxHostView(catalog.makeHost());
    editor.playback_view_ = playback.view();
    editor.plot_tabs_view_ = tabs.view();
    editor.edit_debounce_ = std::chrono::milliseconds(0);  // no waiting for quiet in a test
    editor.dialog_.setOnSave([&editor]() { editor.onSave(); });
  }
  static TransformEditorDialog& dialog(TransformEditorToolbox& editor) {
    return editor.dialog_;
  }
  // One preview pass with every timer released: the catalog is re-read and the trial runs now.
  static void refresh(TransformEditorToolbox& editor) {
    editor.catalog_refresh_ticks_ = 0;
    editor.next_object_preview_refresh_ = {};
    editor.next_preview_refresh_ = {};
    editor.next_series_read_ = {};
    editor.refreshPreview();
  }
  // One preview pass as a tick does it: the timers stay as they are.
  static void tick(TransformEditorToolbox& editor) {
    editor.refreshPreview();
  }
  static void releaseTrialTimer(TransformEditorToolbox& editor) {
    editor.next_preview_refresh_ = {};
  }
  // The Create action as the dialog's tick delivers it.
  static void deliver(TransformEditorToolbox& editor) {
    editor.dialog_.onTick();
  }
  // Create / Modify straight away (the trial is run first, as a tick would have).
  static void save(TransformEditorToolbox& editor) {
    refresh(editor);
    editor.onSave();
  }
  static void showInScene(TransformEditorToolbox& editor) {
    editor.showInScene();
  }
  static int builds(const TransformEditorToolbox& editor) {
    return editor.on_demand_build_count_;
  }
  static std::string paramsJson(TransformEditorToolbox& editor) {
    return editor.buildOnDemandRequest().request.params_json;
  }
  static std::string script(TransformEditorToolbox& editor) {
    return editor.buildOnDemandRequest().request.script;
  }
  static std::string language(TransformEditorToolbox& editor) {
    return editor.buildOnDemandRequest().request.language;
  }
  static void setDebounce(TransformEditorToolbox& editor, std::chrono::milliseconds value) {
    editor.edit_debounce_ = value;
  }
  static nlohmann::json widgets(TransformEditorToolbox& editor) {
    return nlohmann::json::parse(editor.dialog_.widget_data());
  }
  static std::string status(TransformEditorToolbox& editor) {
    return widgets(editor)["statusLabel"]["label"];
  }
};

// What a trial of `return {cropped = ..., count = ...}` reports: the outputs it inferred and one bundle.
const char* const kTrialReport =
    R"({"coverage":{"complete":true},"bundles":[{"requested_ns":0,"stamp_ns":0,"inputs":[],"outputs":{)"
    R"("cropped":{"status":"ok","summary":{"type":"kPointCloud","points":23144}},"count":{"status":"ok","value":42}}}],)"
    R"("outputs":[{"name":"cropped","type":"kPointCloud"},{"name":"count","type":"number"}]})";
const char* const kNoSampleReport = R"({"coverage":{"complete":true},"bundles":[],"outputs":[]})";
const char* const kUnavailableReport =
    R"({"coverage":{"complete":true},"bundles":[{"requested_ns":0,"stamp_ns":0,"inputs":[],"outputs":{)"
    R"("max_z":{"status":"unavailable","reason":"no points in the box"}}}],)"
    R"("outputs":[{"name":"max_z","type":"unknown"}]})";
const char* const kNumberReport =
    R"({"coverage":{"complete":true},"bundles":[{"requested_ns":0,"stamp_ns":0,"inputs":[],"outputs":{)"
    R"("value":{"status":"ok","value":1.83}}}],"outputs":[{"name":"value","type":"number"}]})";

// Objects only: a point cloud and an image, and a 2D-only pair.
const char* const kObjectsReport =
    R"({"coverage":{"complete":true},"bundles":[{"requested_ns":0,"stamp_ns":0,"inputs":[],"outputs":{)"
    R"("cropped":{"status":"ok","summary":{"type":"kPointCloud","points":10}}}}],)"
    R"("outputs":[{"name":"cropped","type":"kPointCloud"}]})";
const char* const kImageReport =
    R"({"coverage":{"complete":true},"bundles":[{"requested_ns":0,"stamp_ns":0,"inputs":[],"outputs":{)"
    R"("img":{"status":"ok","summary":{"type":"kImage"}}}}],)"
    R"("outputs":[{"name":"img","type":"kImage"}]})";

const char* const kMiddleDot = "·";

// An on-demand editor state the way the host stores it: the user's params object plus "__editor".
std::string onDemandConfig(
    const std::string& name, const std::string& params_text = "{\"k\":2}", bool pin = false,
    const std::string& source = "/cloud") {
  nlohmann::json editor = {
      {"output_name", name},
      {"global_code", ""},
      {"function_body", "return { cropped = cloud, count = 1 }"},
      {"sources", nlohmann::json::array({source})},
      {"primary_index", 0},
      {"language", "luau"},
      {"mode", "single"},
      {"kind", "on_demand"},
      {"params_text", params_text},
      {"pin_current_time", pin},
  };
  nlohmann::json params = nlohmann::json::parse(params_text);
  params["__editor"] = editor;
  return params.dump();
}

struct Rig {
  RecordingDpHost dp;
  FakeCatalogHost catalog;
  FakePlaybackHost playback;
  FakePlotTabsHost tabs;
  TransformEditorToolbox editor;
  bool embeds_scene_views = true;  // what the dialog host announces; rebind() after changing it

  Rig() {
    catalog.addObjectTopic("/cloud", "kPointCloud", 5, 0, 5'000'000'000);
    playback.state.current_time_s = 3.0;
    dp.canned_report_json = kTrialReport;
    TransformEditorPreviewTestPeer::bind(editor, dp, catalog, playback, tabs);
  }
  TransformEditorDialog& dialog() {
    return TransformEditorPreviewTestPeer::dialog(editor);
  }
  // The fakes pick their vtable when view() is called, so a test that changes a capability
  // flag rebinds afterwards.
  void rebind() {
    TransformEditorPreviewTestPeer::bind(editor, dp, catalog, playback, tabs, embeds_scene_views);
  }
  void load(const std::string& config) {
    ASSERT_TRUE(dialog().loadConfig(config));
  }
  void refresh() {
    TransformEditorPreviewTestPeer::refresh(editor);
  }
  nlohmann::json widgets() {
    return TransformEditorPreviewTestPeer::widgets(editor);
  }
  // A new recipe built the way a user does: drop an object topic, write the body, let the trial run.
  void newObjectRecipe(const std::string& body = "return { cropped = cloud, count = 1 }") {
    refresh();  // the catalog is read: "/cloud" is a point cloud
    dialog().onItemsDropped("tableSources", {"/cloud"});
    dialog().onCodeChanged("functionText", body);
    refresh();
  }
  // Click Create..., answer the prompt with `name`, and let the tick deliver the action.
  void createNamed(const std::string& name) {
    dialog().onClicked("pushButtonCreate");
    (void)widgets();  // the prompt is a one-shot of this build
    dialog().onTextChanged("createRecipeName", name);
    dialog().onClicked("subDialogAccepted");
    TransformEditorPreviewTestPeer::deliver(editor);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// The trial run
// ---------------------------------------------------------------------------

TEST(TransformEditorTrial, SubmitsWithTheInferFlagAndNoOutputsAndParsesTheReport) {
  Rig rig;
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  ASSERT_EQ(rig.dp.submit_calls, 1);
  EXPECT_EQ(
      rig.dp.submits[0].flags,
      static_cast<std::uint32_t>(PJ_DATA_PROCESSOR_FLAG_EPHEMERAL | PJ_DATA_PROCESSOR_FLAG_INFER_OUTPUTS));
  EXPECT_EQ(rig.dp.submits[0].output_count, 0u) << "the outputs are the host's to infer";
  EXPECT_EQ(rig.dp.submits[0].instant_ns, 3'000'000'000) << "at the cursor";
  const auto& outputs = rig.dialog().inferredOutputs();
  ASSERT_EQ(outputs.size(), 2u);
  EXPECT_EQ(outputs[0].name, "cropped");
  EXPECT_EQ(outputs[0].type, "kPointCloud");
  EXPECT_EQ(outputs[1].name, "count");
  EXPECT_EQ(outputs[1].type, "number");
  EXPECT_EQ(
      TransformEditorPreviewTestPeer::status(rig.editor),
      std::string("count: 42 ") + kMiddleDot + " cropped: point cloud, 23 144 points");
  EXPECT_EQ(rig.dialog().canCreateReason(), "");
}

TEST(TransformEditorTrial, ALoadedRecipeTrustsNoOutputsBeforeATrial) {
  Rig rig;
  rig.load(onDemandConfig("my_filter"));
  EXPECT_TRUE(rig.dialog().inferredOutputs().empty()) << "nothing is trusted before a trial";
  rig.refresh();
  ASSERT_EQ(rig.dialog().inferredOutputs().size(), 2u);
  EXPECT_EQ(rig.dialog().inferredOutputs()[0].name, "cropped");
}

TEST(TransformEditorTrial, NoSampleAtTheCursorRetriesAtTheFirstEntry) {
  Rig rig;
  rig.catalog.addObjectTopic("/late", "kPointCloud", 5, 8'000'000'000, 9'000'000'000);
  rig.dp.report_queue = {kNoSampleReport};  // the first answer: no sample at the cursor; then the canned report
  rig.load(onDemandConfig("my_filter", "{}", false, "/late"));
  rig.refresh();
  ASSERT_EQ(rig.dp.submit_calls, 2);
  EXPECT_EQ(rig.dp.submits[0].instant_ns, 3'000'000'000);
  EXPECT_EQ(rig.dp.submits[1].instant_ns, 8'000'000'000) << "the first entry of /late";
  const std::string status = TransformEditorPreviewTestPeer::status(rig.editor);
  EXPECT_NE(status.find("No sample of /late at the cursor (3.000 s): move the timeline to preview."), std::string::npos)
      << status;
  EXPECT_NE(status.find("Outputs: cropped (point cloud), count (number)"), std::string::npos) << status;
  EXPECT_EQ(status.find("count: 42"), std::string::npos) << "no value of the retried instant";
  EXPECT_EQ(status.find("23 144"), std::string::npos) << "no point count of the retried instant";
  EXPECT_EQ(status.find("previewing"), std::string::npos) << status;
  EXPECT_TRUE(rig.dialog().trialReadout().empty());
  EXPECT_EQ(rig.dialog().canCreateReason(), "");
}

TEST(TransformEditorTrial, AnEmptyReturnNamesTheInputsTheScriptCanRead) {
  Rig rig;
  rig.catalog.addObjectTopic("/lidar_top", "kPointCloud", 5, 0, 5'000'000'000);
  rig.dp.canned_report_json = R"({"error":"the script returned no values"})";
  rig.dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/lidar_top"});
  rig.dialog().onCodeChanged("functionText", "local x = lidar_top");
  rig.refresh();
  EXPECT_EQ(rig.dialog().canCreateReason(), "the script returned no values · inputs are: lidar_top");
}

TEST(TransformEditorTrial, AValueTheScriptReadsButNoInputBindsIsSaidSo) {
  Rig rig;
  rig.catalog.addObjectTopic("/lidar_top", "kPointCloud", 5, 0, 5'000'000'000);
  rig.dp.canned_report_json = R"({"error":"the script returned no values"})";
  rig.dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/lidar_top"});
  rig.dialog().onCodeChanged("functionText", "return value");
  rig.refresh();
  EXPECT_EQ(
      rig.dialog().canCreateReason(),
      "the script returned no values · `value` is not defined here; inputs are: lidar_top");
  // A field called value, or a longer name, is not the variable.
  rig.dialog().onCodeChanged("functionText", "return lidar_top.value + values");
  rig.refresh();
  EXPECT_EQ(rig.dialog().canCreateReason(), "the script returned no values · inputs are: lidar_top");
}

TEST(TransformEditorTrial, OtherHostErrorsAreLeftAsTheyAre) {
  Rig rig;
  rig.dp.canned_report_json = R"({"error":"syntax error near 'end'"})";
  rig.dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  EXPECT_EQ(rig.dialog().canCreateReason(), "syntax error near 'end'");
}

TEST(TransformEditorTrial, NoSampleAnywhereIsAReasonNotACrash) {
  Rig rig;
  rig.catalog.addObjectTopic("/late", "kPointCloud", 5, 8'000'000'000, 9'000'000'000);
  rig.dp.canned_report_json = kNoSampleReport;
  rig.load(onDemandConfig("my_filter", "{}", false, "/late"));
  rig.refresh();
  EXPECT_EQ(rig.dp.submit_calls, 2) << "the cursor, then the first entry; no third try";
  EXPECT_NE(rig.dialog().canCreateReason().find("No sample"), std::string::npos);
  EXPECT_EQ(TransformEditorPreviewTestPeer::widgets(rig.editor)["pushButtonCreate"]["enabled"], false);
}

TEST(TransformEditorTrial, ARunWaitsForQuietAfterAnEditAndThenRuns) {
  Rig rig;
  TransformEditorPreviewTestPeer::setDebounce(rig.editor, std::chrono::seconds(30));
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  EXPECT_EQ(rig.dp.submit_calls, 0);
  EXPECT_EQ(rig.dialog().canCreateReason(), "Waiting for the script to run");
  TransformEditorPreviewTestPeer::setDebounce(rig.editor, std::chrono::milliseconds(0));
  rig.refresh();
  EXPECT_EQ(rig.dp.submit_calls, 1);
  EXPECT_EQ(rig.dialog().canCreateReason(), "");
}

TEST(TransformEditorTrial, CursorMovesRerunAtMostTwiceASecond) {
  Rig rig;
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  ASSERT_EQ(rig.dp.submit_calls, 1);
  rig.playback.state.current_time_s = 4.0;
  TransformEditorPreviewTestPeer::tick(rig.editor);
  EXPECT_EQ(rig.dp.submit_calls, 1) << "inside the 500 ms period nothing is submitted";
  TransformEditorPreviewTestPeer::releaseTrialTimer(rig.editor);
  TransformEditorPreviewTestPeer::tick(rig.editor);
  ASSERT_EQ(rig.dp.submit_calls, 2);
  EXPECT_EQ(rig.dp.submits[1].instant_ns, 4'000'000'000);
  EXPECT_EQ(rig.dialog().canCreateReason(), "") << "a cursor move is not an edit: Create stays enabled";
}

TEST(TransformEditorTrial, SeriesOnlyInputsRunNoTrial) {
  Rig rig;
  rig.load(R"({"output_name":"dbl","global_code":"","function_body":"return value*2","sources":["a/x"]})");
  rig.refresh();
  EXPECT_EQ(rig.dp.submit_calls, 0);
  EXPECT_FALSE(rig.dialog().isOnDemand());
  EXPECT_EQ(rig.dp.last_kind, "transform") << "the existing ephemeral transform preview";
}

TEST(TransformEditorTrial, ParseTrialReportReadsOutputsUnknownsAndErrors) {
  const TrialReport ok = parseTrialReport(kTrialReport);
  EXPECT_TRUE(ok.has_sample);
  EXPECT_FALSE(ok.has_unknown);
  EXPECT_EQ(ok.outputs.size(), 2u);
  const TrialReport unknown = parseTrialReport(kUnavailableReport);
  EXPECT_TRUE(unknown.has_unknown);
  EXPECT_EQ(unknown.outputs[0].type, "unknown");
  EXPECT_FALSE(parseTrialReport(kNoSampleReport).has_sample);
  EXPECT_EQ(parseTrialReport(R"({"error":"boom"})").error, "boom");
  EXPECT_EQ(
      parseTrialReport(R"({"bundles":[{"outputs":{"o":{"status":"error","reason":"bad return"}}}],"outputs":[]})")
          .error,
      "bad return");
}

TEST(TransformEditorTrial, StatusReadsUnavailableWithItsReason) {
  EXPECT_EQ(parseTrialReport(kUnavailableReport).summary, "max_z: unavailable (no points in the box)");
  EXPECT_EQ(parseTrialReport(kNumberReport).summary, "value: 1.83");
  EXPECT_EQ(parseTrialReport(kNumberReport).readout, "value: 1.83");
  EXPECT_EQ(parseTrialReport(kUnavailableReport).readout, "") << "an unknown-typed output is not a number output";
}

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------

TEST(TransformEditorCreate, ModifySendsTheInferFlagTheInferredOutputsParamsEditorStateAndInstant) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", "{\"k\":2}", /*pin=*/true));
  TransformEditorPreviewTestPeer::save(rig.editor);

  ASSERT_EQ(rig.dp.persistent_creates, 1);
  ASSERT_EQ(rig.dp.created.size(), 1u);
  EXPECT_EQ(rig.dp.created[0].kind, "on_demand");
  EXPECT_EQ(rig.dp.created[0].id, "my_filter");
  EXPECT_EQ(rig.dp.last_create_v2_label, "my_filter");
  EXPECT_EQ(rig.dp.last_create_v2_flags, static_cast<std::uint32_t>(PJ_DATA_PROCESSOR_FLAG_INFER_OUTPUTS))
      << "the user's own recipe is NOT ephemeral nor history-exempt";
  EXPECT_EQ(rig.dp.last_create_v2_inputs, std::vector<std::string>{"/cloud"});
  EXPECT_EQ(rig.dp.created[0].outputs, (std::vector<std::string>{"cropped", "count"}));
  EXPECT_EQ(rig.dp.last_output_types, (std::vector<std::string>{"kPointCloud", "number"}));
  EXPECT_EQ(rig.dp.last_create_v2_time_flags, static_cast<std::uint32_t>(PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT));
  EXPECT_EQ(rig.dp.last_create_v2_instant_ns, 3'000'000'000);  // playhead 3 s through the shared toRawNs
  EXPECT_NE(rig.dp.last_create_v2_script.find("local inputs, params = ..."), std::string::npos);
  EXPECT_NE(rig.dp.last_create_v2_script.find("local cloud = inputs[\"/cloud\"]"), std::string::npos)
      << "one local per input, named by its Var";

  const auto params = nlohmann::json::parse(rig.dp.last_create_v2_params_json);
  EXPECT_EQ(params["k"], 2);
  ASSERT_TRUE(params.contains("__editor"));
  EXPECT_EQ(params["__editor"]["kind"], "on_demand");
  EXPECT_EQ(params["__editor"]["output_name"], "my_filter");
  EXPECT_EQ(params["__editor"]["vars"], nlohmann::json::array({"cloud"}));
  EXPECT_FALSE(params["__editor"].contains("outputs")) << "a trial infers them on load";
}

TEST(TransformEditorCreate, WithoutPinTheRecipeCarriesNoInstant) {
  Rig rig;
  rig.load(onDemandConfig("my_filter"));
  TransformEditorPreviewTestPeer::save(rig.editor);
  ASSERT_EQ(rig.dp.persistent_creates, 1);
  EXPECT_EQ(rig.dp.last_create_v2_time_flags, 0u);
}

TEST(TransformEditorCreate, ModifyKeepsTheNameAndAsksNothing) {
  Rig rig;
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  auto widgets = rig.widgets();
  EXPECT_EQ(widgets["pushButtonCreate"]["button_text"], "Modify");
  EXPECT_EQ(widgets["pushButtonCreate"]["enabled"], true);
  rig.dialog().onClicked("pushButtonCreate");
  EXPECT_FALSE(rig.widgets().contains("__request_sub_dialog")) << "Modify has no name prompt";
  rig.dialog().onClicked("pushButtonCreate");
  TransformEditorPreviewTestPeer::deliver(rig.editor);
  rig.dialog().onClicked("pushButtonCreate");
  TransformEditorPreviewTestPeer::deliver(rig.editor);
  ASSERT_EQ(rig.dp.created.size(), 2u);
  EXPECT_EQ(rig.dp.created[0].id, "my_filter");
  EXPECT_EQ(rig.dp.created[1].id, "my_filter");
}

TEST(TransformEditorCreate, CreateOpensThePromptWithTheSummaryAndOkCreatesUnderTheGivenName) {
  Rig rig;
  rig.newObjectRecipe();
  auto widgets = rig.widgets();
  EXPECT_EQ(widgets["pushButtonCreate"]["button_text"], "Create…");
  ASSERT_EQ(widgets["pushButtonCreate"]["enabled"], true);

  rig.dialog().onClicked("pushButtonCreate");
  widgets = rig.widgets();
  ASSERT_TRUE(widgets.contains("__request_sub_dialog"));
  EXPECT_NE(std::string(widgets["__request_sub_dialog"]["ui"]).find("createRecipeName"), std::string::npos);
  EXPECT_EQ(widgets["createRecipeName"]["text"], "cropped") << "prefilled with the first inferred output";
  EXPECT_EQ(
      widgets["createRecipeSummary"]["label"], "point cloud `cropped`, series `count`; computed per /cloud frame");
  EXPECT_EQ(rig.dp.persistent_creates, 0) << "nothing is created before OK";

  rig.dialog().onTextChanged("createRecipeName", "my_recipe");
  rig.dialog().onClicked("subDialogAccepted");
  TransformEditorPreviewTestPeer::deliver(rig.editor);
  ASSERT_EQ(rig.dp.persistent_creates, 1);
  EXPECT_EQ(rig.dp.created[0].id, "my_recipe");
  EXPECT_EQ(rig.dp.created[0].kind, "on_demand");
  EXPECT_EQ(rig.dp.last_create_v2_flags, static_cast<std::uint32_t>(PJ_DATA_PROCESSOR_FLAG_INFER_OUTPUTS));
  EXPECT_EQ(rig.dp.created[0].outputs, (std::vector<std::string>{"cropped", "count"}));
}

TEST(TransformEditorCreate, ThePromptSummarizesASeriesRecipe) {
  Rig rig;
  rig.dp.canned_report_json = kNumberReport;
  rig.newObjectRecipe("return cloud:count()");
  rig.dialog().onClicked("pushButtonCreate");
  EXPECT_EQ(rig.widgets()["createRecipeSummary"]["label"], "series `value`; computed per /cloud frame");
}

TEST(TransformEditorCreate, TheNameMustBeNonEmptyNotAnInputAndWithoutDoubleUnderscore) {
  Rig rig;
  rig.newObjectRecipe();
  EXPECT_NE(rig.dialog().createNameError(""), "");
  EXPECT_NE(rig.dialog().createNameError("   "), "");
  EXPECT_NE(rig.dialog().createNameError("/cloud"), "") << "an input name";
  EXPECT_NE(rig.dialog().createNameError("a__b"), "");
  EXPECT_EQ(rig.dialog().createNameError("good_name"), "");

  for (const char* bad : {"", "/cloud", "a__b"}) {
    rig.createNamed(bad);
    EXPECT_EQ(rig.dp.persistent_creates, 0) << bad;
    const auto widgets = rig.widgets();
    EXPECT_TRUE(widgets.contains("__request_sub_dialog")) << "the prompt opens again for '" << bad << "'";
    EXPECT_NE(std::string(widgets["createRecipeNote"]["label"]), "");
  }
  rig.dialog().onTextChanged("createRecipeName", "good_name");
  rig.dialog().onClicked("subDialogAccepted");
  TransformEditorPreviewTestPeer::deliver(rig.editor);
  EXPECT_EQ(rig.dp.persistent_creates, 1);
}

TEST(TransformEditorCreate, ReplacingAnOwnRecipeNeedsASecondOk) {
  Rig rig;
  rig.dp.live_ids = {"mine"};
  rig.dp.canned_config_json = R"({"kind":"on_demand","params":{"k":1,"__editor":{"output_name":"mine"}}})";
  rig.newObjectRecipe();
  rig.dialog().onClicked("pushButtonCreate");
  rig.dialog().onTextChanged("createRecipeName", "mine");
  rig.dialog().onClicked("subDialogAccepted");
  TransformEditorPreviewTestPeer::deliver(rig.editor);
  EXPECT_EQ(rig.dp.persistent_creates, 0) << "the first OK only warns";
  auto widgets = rig.widgets();
  ASSERT_TRUE(widgets.contains("__request_sub_dialog"));
  EXPECT_NE(std::string(widgets["createRecipeNote"]["label"]).find("replace"), std::string::npos);
  rig.dialog().onTextChanged("createRecipeName", "mine");
  rig.dialog().onClicked("subDialogAccepted");
  TransformEditorPreviewTestPeer::deliver(rig.editor);
  ASSERT_EQ(rig.dp.created.size(), 1u) << "the second OK replaces";
  EXPECT_EQ(rig.dp.created[0].id, "mine");
}

TEST(TransformEditorCreate, ARecipeOfSomeoneElseIsNotTheEditorsToReplace) {
  Rig rig;
  rig.dp.live_ids = {"theirs"};
  rig.dp.canned_config_json = R"({"kind":"on_demand","params":{"k":1}})";  // no editor state: not ours
  rig.newObjectRecipe();
  rig.dialog().onClicked("pushButtonCreate");
  EXPECT_EQ(rig.widgets()["createRecipeNote"]["label"], "");
  EXPECT_FALSE(rig.dialog().canCreateReason().size());
}

TEST(TransformEditorCreate, ASeriesRecipeStillGoesThroughCreateTransform) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"a/x"});
  rig.dialog().onCodeChanged("functionText", "return value*2");
  rig.refresh();
  EXPECT_FALSE(rig.dialog().isOnDemand());
  rig.createNamed("dbl");
  EXPECT_EQ(rig.dp.create_v2_calls, 0);
  EXPECT_EQ(rig.dp.last_kind, "transform");
  EXPECT_EQ(rig.dp.last_id, "dbl");
  EXPECT_EQ(rig.dp.last_outputs, std::vector<std::string>{"dbl"});
  EXPECT_EQ(rig.dp.submit_calls, 0);
}

TEST(TransformEditorCreate, SeveralReturnedSeriesAreNamedNameSlashA) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"a/x"});
  rig.dialog().onCodeChanged("functionText", "return value, value * 2");
  rig.refresh();
  // The preview node has as many outputs as the body returns values.
  EXPECT_EQ(rig.dp.last_outputs.size(), 2u);
  rig.createNamed("mix");
  EXPECT_EQ(rig.dp.last_kind, "transform");
  EXPECT_EQ(rig.dp.last_id, "mix/a");
  EXPECT_EQ(rig.dp.last_outputs, (std::vector<std::string>{"mix/a", "mix/b"}));
}

TEST(TransformEditorCreate, ASavedCommaSeparatedNameKeepsItsOutputs) {
  Rig rig;
  rig.load(R"({"output_name":"roll,pitch","global_code":"","function_body":"return r, p","sources":["a/x"]})");
  TransformEditorPreviewTestPeer::save(rig.editor);
  EXPECT_EQ(rig.dp.last_outputs, (std::vector<std::string>{"roll", "pitch"}));
}

TEST(TransformEditorCreate, DisabledReasonsGoOnTheStatusLineAndTheButton) {
  {
    Rig rig;  // no input
    rig.refresh();
    const std::string reason = "Add an input (drag & drop a series or an object topic)";
    EXPECT_EQ(rig.dialog().canCreateReason(), reason);
    const auto widgets = rig.widgets();
    EXPECT_EQ(widgets["pushButtonCreate"]["enabled"], false);
    EXPECT_EQ(widgets["statusLabel"]["label"], reason);
    EXPECT_EQ(widgets["pushButtonCreate"]["valid_tooltip"], reason);
  }
  {
    Rig rig;  // no body
    rig.refresh();
    rig.dialog().onItemsDropped("tableSources", {"/cloud"});
    rig.dialog().onCodeChanged("functionText", "");
    rig.refresh();
    EXPECT_EQ(rig.dialog().canCreateReason(), "Write your function body");
    EXPECT_EQ(TransformEditorPreviewTestPeer::status(rig.editor), "Write your function body");
  }
  {
    Rig rig;  // a host without the on-demand surfaces
    rig.dp.supports_v2 = false;
    rig.rebind();
    rig.load(onDemandConfig("my_filter"));
    rig.refresh();
    EXPECT_NE(rig.dialog().canCreateReason().find("SDK 0.36"), std::string::npos);
    EXPECT_EQ(rig.widgets()["pushButtonCreate"]["enabled"], false);
    TransformEditorPreviewTestPeer::save(rig.editor);
    EXPECT_EQ(rig.dp.persistent_creates, 0);
    EXPECT_EQ(rig.dp.submit_calls, 0);
  }
  {
    Rig rig;  // an output unavailable at this instant: its type is not known yet
    rig.dp.canned_report_json = kUnavailableReport;
    rig.load(onDemandConfig("my_filter"));
    rig.refresh();
    EXPECT_NE(rig.dialog().canCreateReason().find("unavailable at this instant"), std::string::npos);
    EXPECT_NE(
        TransformEditorPreviewTestPeer::status(rig.editor).find("max_z: unavailable (no points in the box)"),
        std::string::npos);
    TransformEditorPreviewTestPeer::save(rig.editor);
    EXPECT_EQ(rig.dp.persistent_creates, 0);
    EXPECT_EQ(rig.dp.created.size(), 0u);
    rig.dp.canned_report_json = kTrialReport;  // the cursor reaches a frame with points
    rig.refresh();
    EXPECT_EQ(rig.dialog().canCreateReason(), "");
  }
  {
    Rig rig;  // a script error: the host's message
    rig.dp.canned_report_json = R"({"error":"attempt to index nil with 'crop_box'"})";
    rig.dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
    rig.load(onDemandConfig("my_filter"));
    rig.refresh();
    EXPECT_EQ(rig.dialog().canCreateReason(), "attempt to index nil with 'crop_box'");
    EXPECT_EQ(TransformEditorPreviewTestPeer::status(rig.editor), "attempt to index nil with 'crop_box'");
    TransformEditorPreviewTestPeer::save(rig.editor);
    EXPECT_EQ(rig.dp.persistent_creates, 0);
  }
  {
    Rig rig;  // the host rejects the script
    rig.dp.fail_validate = true;
    rig.load(onDemandConfig("my_filter"));
    rig.refresh();
    EXPECT_NE(rig.dialog().canCreateReason().find("compile error"), std::string::npos);
    EXPECT_EQ(rig.dp.submit_calls, 0);
  }
  {
    Rig rig;  // params must be a JSON object, and not use the editor's own key
    rig.load(onDemandConfig("my_filter"));
    rig.dialog().onTextChanged("paramsLineEdit", "{\"__editor\":1}");
    rig.refresh();
    EXPECT_NE(rig.dialog().canCreateReason().find("reserved"), std::string::npos);
    TransformEditorPreviewTestPeer::save(rig.editor);
    EXPECT_EQ(rig.dp.persistent_creates, 0);
  }
}

TEST(TransformEditorCreate, ACreateWithNoSuccessfulTrialIsRefusedEvenWhenAskedDirectly) {
  Rig rig;
  rig.dp.fail_submit = true;
  rig.load(onDemandConfig("my_filter"));
  TransformEditorPreviewTestPeer::save(rig.editor);
  EXPECT_EQ(rig.dp.persistent_creates, 0);
}

TEST(TransformEditorCreate, ShowInSceneOpensA3dTabForPointCloudsAnd2dForAnnotations) {
  for (const auto& [type, scene_kind] :
       {std::pair<std::string, std::string>{"kPointCloud", "3d"},
        std::pair<std::string, std::string>{"kImageAnnotations", "2d"}}) {
    Rig rig;
    rig.dp.canned_report_json = R"({"bundles":[{"outputs":{"out":{"status":"ok","summary":{"type":")" + type +
                                R"("}}}}],"outputs":[{"name":"out","type":")" + type + R"("}]})";
    rig.load(onDemandConfig("my_filter"));
    TransformEditorPreviewTestPeer::save(rig.editor);
    ASSERT_EQ(rig.dp.persistent_creates, 1);
    const auto widgets = rig.widgets();
    EXPECT_EQ(widgets["buttonShowScene"]["visible"], true);
    EXPECT_EQ(widgets["buttonShowScene"]["button_text"], scene_kind == "2d" ? "Show in 2D" : "Show in 3D");
    TransformEditorPreviewTestPeer::showInScene(rig.editor);
    ASSERT_EQ(rig.tabs.tabs.size(), 1u);
    EXPECT_EQ(rig.tabs.tabs[0].kind, scene_kind);
    ASSERT_EQ(rig.tabs.tabs[0].topics.size(), 1u);
    EXPECT_EQ(rig.tabs.tabs[0].topics[0].topic, "out");
  }
}

TEST(TransformEditorCreate, NoSceneButtonWithoutSceneTabsOrObjectOutputs) {
  {
    Rig rig;
    rig.tabs.null_tail = true;  // a host with no scene workspace
    rig.rebind();
    rig.load(onDemandConfig("my_filter"));
    TransformEditorPreviewTestPeer::save(rig.editor);
    EXPECT_EQ(rig.widgets()["buttonShowScene"]["visible"], false);
  }
  {
    Rig rig;  // numbers only: nothing to show in a scene
    rig.dp.canned_report_json = kNumberReport;
    rig.load(onDemandConfig("my_filter"));
    TransformEditorPreviewTestPeer::save(rig.editor);
    EXPECT_EQ(rig.dp.persistent_creates, 1);
    EXPECT_EQ(rig.widgets()["buttonShowScene"]["visible"], false);
  }
}

TEST(TransformEditorCreate, ShowInSceneAttachesWithTheInputsDataset) {
  Rig rig;
  rig.catalog.addObjectTopic("/a", "kPointCloud", 1, 0, 1'000'000'000, "{}", "runA");
  rig.catalog.addObjectTopic("/b", "kPointCloud", 1, 0, 1'000'000'000, "{}", "runB");
  rig.dp.canned_report_json = R"({"bundles":[{"outputs":{"out":{"status":"ok","summary":{"type":"kPointCloud"}}}}],)"
                              R"("outputs":[{"name":"out","type":"kPointCloud"}]})";
  rig.load(onDemandConfig("my_filter", "{}", false, "runA:/a"));
  TransformEditorPreviewTestPeer::save(rig.editor);
  TransformEditorPreviewTestPeer::showInScene(rig.editor);
  ASSERT_EQ(rig.tabs.tabs.size(), 1u);
  ASSERT_EQ(rig.tabs.tabs[0].topics.size(), 1u);
  EXPECT_EQ(rig.tabs.tabs[0].topics[0].topic, "out");
  EXPECT_EQ(rig.tabs.tabs[0].topics[0].dataset, "runA");
}

TEST(TransformEditorCreate, PreviewTicksReuseTheResolvedBuild) {
  Rig rig;
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  const int after_first = TransformEditorPreviewTestPeer::builds(rig.editor);
  ASSERT_GE(after_first, 1);
  const int validations = rig.dp.validate_calls;
  ASSERT_GE(validations, 1);
  for (int i = 0; i < 5; ++i) {
    TransformEditorPreviewTestPeer::tick(rig.editor);
  }
  EXPECT_EQ(TransformEditorPreviewTestPeer::builds(rig.editor), after_first) << "unchanged form: cached build";
  EXPECT_EQ(rig.dp.validate_calls, validations) << "the host compiles the script once per form revision";
  rig.dialog().onTextChanged("paramsLineEdit", "{\"k\":3}");
  EXPECT_NE(TransformEditorPreviewTestPeer::paramsJson(rig.editor).find("\"k\":3"), std::string::npos);
  EXPECT_EQ(TransformEditorPreviewTestPeer::builds(rig.editor), after_first + 1);
  rig.refresh();  // a catalog re-read drops the cache
  EXPECT_EQ(TransformEditorPreviewTestPeer::builds(rig.editor), after_first + 2);
}

// ---------------------------------------------------------------------------
// Saved states keep loading
// ---------------------------------------------------------------------------

TEST(TransformEditorSavedState, AV10TransformStateLoadsAndKeepsItsShape) {
  Rig rig;
  rig.load(
      R"({"output_name":"dbl","global_code":"","function_body":"return value*2","sources":["a/x","b/y"],)"
      R"("primary_index":1,"language":"luau","mode":"single"})");
  EXPECT_FALSE(rig.dialog().isOnDemand());
  EXPECT_EQ(rig.dialog().outputName(), "dbl");
  EXPECT_EQ(rig.dialog().sourceSeries(), "b/y");
  EXPECT_EQ(rig.dialog().extraSources(), std::vector<std::string>{"a/x"});
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"v1", "value"})) << "series keep value, v1...";
  const auto saved = nlohmann::json::parse(rig.dialog().saveConfig());
  EXPECT_FALSE(saved.contains("kind"));
  EXPECT_FALSE(saved.contains("outputs"));
  EXPECT_FALSE(saved.contains("__editor"));
  EXPECT_EQ(saved["function_body"], "return value*2");
  EXPECT_EQ(rig.widgets()["pushButtonCreate"]["button_text"], "Modify") << "opened from Custom Topics: name locked";
}

TEST(TransformEditorSavedState, AStateWithKindLoadsAsAnOnDemandRecipe) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", "{\"k\":2}"));
  rig.refresh();
  EXPECT_TRUE(rig.dialog().isOnDemand());
  EXPECT_EQ(rig.dialog().outputName(), "my_filter");
  EXPECT_EQ(rig.dialog().sources(), std::vector<std::string>{"/cloud"});
  EXPECT_EQ(rig.dialog().paramsText(), "{\"k\":2}");
  EXPECT_EQ(rig.dialog().variableNames(), std::vector<std::string>{"cloud"});
  EXPECT_FALSE(nlohmann::json::parse(rig.dialog().saveConfig()).contains("outputs")) << "a trial infers them";
  EXPECT_TRUE(rig.widgets()["advancedPane"]["visible"].get<bool>()) << "params are set: the disclosure opens";
}

TEST(TransformEditorSavedState, ASavedOnDemandKindStaysOnDemandUntilTheInputsChange) {
  Rig rig;
  nlohmann::json editor = {
      {"output_name", "n"},
      {"function_body", "return {r = inputs[\"a/x\"]}"},
      {"sources", nlohmann::json::array({"a/x"})},
      {"kind", "on_demand"}};
  nlohmann::json params = {{"__editor", editor}};
  rig.load(params.dump());
  EXPECT_TRUE(rig.dialog().isOnDemand()) << "its script is an on-demand chunk whatever it reads";
  rig.dialog().onItemsDropped("tableSources", {"b/y"});
  EXPECT_FALSE(rig.dialog().isOnDemand());
}

TEST(TransformEditorSavedState, AStateSavedByThisEditorLoadsBackWithItsVars) {
  Rig rig;
  rig.catalog.addObjectTopic("/a/lidar", "kPointCloud", 1, 0, 1);
  rig.catalog.addObjectTopic("/b/lidar", "kPointCloud", 1, 0, 1);
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/a/lidar", "/b/lidar"});
  rig.dialog().onCodeChanged("functionText", "return a_cloud:count() + b_cloud:count()");
  rig.dialog().bindSnippetInputs({{"b_cloud", "kPointCloud"}});
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"b_cloud", "lidar"}));
  const std::string saved = rig.dialog().saveConfig();
  Rig other;
  other.catalog.addObjectTopic("/a/lidar", "kPointCloud", 1, 0, 1);
  other.catalog.addObjectTopic("/b/lidar", "kPointCloud", 1, 0, 1);
  other.load(saved);
  other.refresh();
  EXPECT_EQ(other.dialog().variableNames(), (std::vector<std::string>{"b_cloud", "lidar"}));
}

// ---------------------------------------------------------------------------
// Layout and variables
// ---------------------------------------------------------------------------

TEST(TransformEditorLayout, TheRemovedWidgetsAreGone) {
  Rig rig;
  const std::string ui = rig.dialog().ui_content();
  for (const char* removed :
       {"tableOutputs", "outputNameEdit", "outputTypeCombo", "buttonAddOutput", "engineLabel", "nameLineEdit",
        "outputTimeseriesHeader", "onDemandReportPreview"}) {
    EXPECT_EQ(ui.find(std::string("name=\"") + removed + "\""), std::string::npos) << removed;
  }
  rig.newObjectRecipe();
  const auto widgets = rig.widgets();
  for (const char* removed :
       {"tableOutputs", "outputNameEdit", "outputTypeCombo", "buttonAddOutput", "engineLabel", "nameLineEdit",
        "onDemandReportPreview"}) {
    EXPECT_FALSE(widgets.contains(removed)) << removed << " is not in the widget data";
  }
  for (const char* wanted :
       {"tableSources", "statusLabel", "pushButtonCreate", "framePlotPreview", "buttonAdvanced", "advancedPane"}) {
    EXPECT_NE(ui.find(std::string("name=\"") + wanted + "\""), std::string::npos) << wanted;
  }
  // Inputs are added by drag and drop only: no picker, no Add input, no spacer holding space under the table.
  for (const char* gone : {"objectTopicCombo", "buttonAddObjectTopic", "objectTopicLayout", "leftSpacer"}) {
    EXPECT_EQ(ui.find(std::string("name=\"") + gone + "\""), std::string::npos) << gone;
    EXPECT_FALSE(widgets.contains(gone)) << gone;
  }
}

TEST(TransformEditorLayout, TheInputsTableHasAVisibleHeaderAndReadableTypes) {
  Rig rig;
  rig.catalog.addObjectTopic("/img", "kImage", 3, 0, 1'000'000'000);
  rig.catalog.addObjectTopic("/ann", "kImageAnnotations", 3, 0, 1'000'000'000);
  rig.catalog.addObjectTopic("/scene", "kSceneEntities", 3, 0, 1'000'000'000);
  rig.catalog.addObjectTopic("/tf", "kFrameTransforms", 3, 0, 1'000'000'000);
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/cloud", "/img", "/ann", "/scene", "/tf", "pose/x"});
  const auto widgets = rig.widgets();
  EXPECT_EQ(widgets["tableSources"]["headers"], nlohmann::json::array({"", "Input", "Var", "Type"}));
  const auto rows = widgets["tableSources"]["rows"];
  EXPECT_EQ(rows[0][3], "point cloud");
  EXPECT_EQ(rows[1][3], "image");
  EXPECT_EQ(rows[2][3], "image annotations");
  EXPECT_EQ(rows[3][3], "scene");
  EXPECT_EQ(rows[4][3], "transforms");
  EXPECT_EQ(rows[5][3], "number");
  EXPECT_EQ(
      rig.dialog().ui_content().find("<attribute name=\"horizontalHeaderVisible\">\n                   <bool>false"),
      std::string::npos);
  const std::string text = rig.dialog().ui_content();
  const std::size_t table = text.find("name=\"tableSources\"");
  const std::size_t header = text.find("horizontalHeaderVisible", table);
  ASSERT_NE(header, std::string::npos);
  EXPECT_NE(text.substr(header, 80).find("<bool>true</bool>"), std::string::npos) << "the header is visible";
}

TEST(TransformEditorLayout, ObjectTopicsDroppedFromTheDatasetsTreeAreTheOnlyWayToAddThem) {
  Rig rig;
  rig.catalog.addObjectTopic("/img", "kImage", 3, 0, 1'000'000'000);
  rig.catalog.addObjectTopic("/a", "kPointCloud", 1, 0, 1'000'000'000, "{}", "runA");
  rig.refresh();
  EXPECT_TRUE(rig.widgets()["tableSources"]["rows"].empty());
  EXPECT_TRUE(rig.dialog().onItemsDropped("tableSources", {"/img", "runA:/a", "/img"}));
  EXPECT_EQ(rig.dialog().sources(), (std::vector<std::string>{"/img", "runA:/a"})) << "no duplicates";
  const auto rows = rig.widgets()["tableSources"]["rows"];
  EXPECT_EQ(rows[0][3], "image");
  EXPECT_EQ(rows[1][3], "point cloud");
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"img", "a"}));
  // The removed button is not a click target any more.
  EXPECT_FALSE(rig.dialog().onClicked("buttonAddObjectTopic"));
}

TEST(TransformEditorLayout, TheInputsTableKeepsTheLeftPaneHeight) {
  Rig rig;
  const std::string ui = rig.dialog().ui_content();
  const std::size_t table = ui.find("name=\"tableSources\"");
  const std::size_t advanced = ui.find("name=\"buttonAdvanced\"");
  ASSERT_NE(table, std::string::npos);
  ASSERT_NE(advanced, std::string::npos);
  const std::string between = ui.substr(table, advanced - table);
  EXPECT_NE(between.find("vsizetype=\"Expanding\""), std::string::npos);
  EXPECT_NE(between.find("<verstretch>1</verstretch>"), std::string::npos);
  EXPECT_EQ(between.find("<spacer"), std::string::npos);
  const std::size_t left_end = ui.find("name=\"rightWidget\"");
  EXPECT_EQ(ui.substr(advanced, left_end - advanced).find("<spacer"), std::string::npos)
      << "nothing after Advanced soaks up the height";
}

TEST(TransformEditorLayout, TheFunctionHeaderIsElidedAndTheRightPaneKeepsRoomForItsButtons) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/cloud"});
  rig.dialog().onCodeChanged("functionText", "return {}");
  EXPECT_EQ(rig.widgets()["functionTitle"]["text"], "function( cloud )");
  rig.catalog.addObjectTopic("/a/a_really_long_topic_leaf_name", "kPointCloud", 1, 0, 1);
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/a/a_really_long_topic_leaf_name"});
  const std::string title = rig.widgets()["functionTitle"]["text"];
  EXPECT_LE(title.size(), 28u) << title;
  EXPECT_EQ(title.substr(title.size() - 3), "...");
  const std::string ui = rig.dialog().ui_content();
  const std::size_t right = ui.find("name=\"rightWidget\"");
  ASSERT_NE(right, std::string::npos);
  EXPECT_NE(ui.substr(right, 200).find("<width>480</width>"), std::string::npos);
}

TEST(TransformEditorBody, TheUntouchedDefaultFollowsTheFirstObjectInput) {
  Rig rig;
  rig.refresh();
  EXPECT_EQ(rig.dialog().functionBody(), "return value");
  rig.dialog().onItemsDropped("tableSources", {"/cloud"});
  EXPECT_EQ(rig.dialog().functionBody(), "return cloud");
  // The host echoes the code it was given: not an edit.
  rig.dialog().onCodeChanged("functionText", "return cloud");
  rig.dialog().onClicked("buttonClearSources");
  EXPECT_EQ(rig.dialog().functionBody(), "return value") << "back to the template with no object input";
}

TEST(TransformEditorBody, AnEmptyBodyIsFilledWhenTheRecipeBecomesOnDemand) {
  Rig rig;
  rig.refresh();
  rig.dialog().onCodeChanged("functionText", "");
  EXPECT_EQ(rig.dialog().functionBody(), "");
  rig.dialog().onItemsDropped("tableSources", {"/cloud"});
  EXPECT_EQ(rig.dialog().functionBody(), "return cloud");
}

TEST(TransformEditorBody, AnEditedBodyIsNeverTouched) {
  Rig rig;
  rig.refresh();
  rig.dialog().onCodeChanged("functionText", "return value * 2");
  rig.dialog().onItemsDropped("tableSources", {"/cloud"});
  EXPECT_EQ(rig.dialog().functionBody(), "return value * 2");
  rig.dialog().onClicked("buttonClearSources");
  EXPECT_EQ(rig.dialog().functionBody(), "return value * 2");

  // An automatic body the user then edits stays theirs, also after the inputs go.
  Rig second;
  second.refresh();
  second.dialog().onItemsDropped("tableSources", {"/cloud"});
  ASSERT_EQ(second.dialog().functionBody(), "return cloud");
  second.dialog().onCodeChanged("functionText", "return { n = 1 }");
  second.dialog().onClicked("buttonClearSources");
  EXPECT_EQ(second.dialog().functionBody(), "return { n = 1 }");

  // Clearing the editor by hand while an object input is there leaves it empty.
  Rig third;
  third.refresh();
  third.dialog().onItemsDropped("tableSources", {"/cloud"});
  third.dialog().onCodeChanged("functionText", "");
  EXPECT_EQ(third.dialog().functionBody(), "");
}

TEST(TransformEditorBody, SeriesOnlyInputsKeepReturnValue) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"a/x"});
  EXPECT_EQ(rig.dialog().functionBody(), "return value");
}

TEST(TransformEditorLayout, TheAdvancedDisclosureIsCollapsedUntilToggled) {
  Rig rig;
  rig.newObjectRecipe();
  auto widgets = rig.widgets();
  EXPECT_EQ(widgets["buttonAdvanced"]["visible"], true);
  EXPECT_EQ(widgets["advancedPane"]["visible"], false);
  EXPECT_EQ(widgets["buttonAdvanced"]["button_text"], "Advanced >");
  rig.dialog().onClicked("buttonAdvanced");
  widgets = rig.widgets();
  EXPECT_EQ(widgets["advancedPane"]["visible"], true);
  EXPECT_EQ(widgets["buttonAdvanced"]["button_text"], "Advanced v");
  rig.dialog().onClicked("buttonAdvanced");
  EXPECT_EQ(rig.widgets()["advancedPane"]["visible"], false);
}

TEST(TransformEditorLayout, NothingAdvancedIsOfferedToSeriesOnlyRecipes) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"a/x"});
  const auto widgets = rig.widgets();
  EXPECT_EQ(widgets["buttonAdvanced"]["visible"], false);
  EXPECT_EQ(widgets["advancedPane"]["visible"], false);
}

TEST(TransformEditorVars, DefaultToTheSanitizedLeafWithSuffixesAndNoKeywords) {
  Rig rig;
  rig.catalog.addObjectTopic("/a/lidar_top", "kPointCloud", 1, 0, 1);
  rig.catalog.addObjectTopic("/b/lidar_top", "kPointCloud", 1, 0, 1);
  rig.catalog.addObjectTopic("/end", "kPointCloud", 1, 0, 1);
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/a/lidar_top", "/b/lidar_top", "/end", "pose/x"});
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"lidar_top", "lidar_top_2", "end_", "x"}));
  const auto rows = rig.widgets()["tableSources"]["rows"];
  EXPECT_EQ(rows[1][2], "lidar_top_2");
  EXPECT_EQ(rig.widgets()["functionTitle"]["text"], "function( lidar_top, lida...") << "elided to fit the band";
  EXPECT_EQ(rig.dialog().variableBindings()[1].key, "/b/lidar_top");
}

TEST(TransformEditorVars, ASeriesOnlyRecipeKeepsValueAndV1) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"a/x", "b/y"});
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"value", "v1"}));
  EXPECT_EQ(rig.widgets()["functionTitle"]["text"], "function( time, value, v1 )");
  rig.dialog().onTableRadioSelected("tableSources", 1);
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"v1", "value"}));
}

TEST(TransformEditorVars, TheBodyReadsEveryInputByItsVarAndStillByItsPath) {
  Rig rig;
  rig.newObjectRecipe();
  const std::string script = TransformEditorPreviewTestPeer::script(rig.editor);
  EXPECT_NE(script.find("local cloud = inputs[\"/cloud\"]"), std::string::npos) << script;
  EXPECT_NE(script.find("[\"/cloud\"] = inputs[\"/cloud\"]"), std::string::npos) << "inputs[\"/topic\"] keeps working";
  EXPECT_LT(script.find("local cloud = "), script.find("return { cropped"));
  EXPECT_EQ(TransformEditorPreviewTestPeer::language(rig.editor), "luau");
}

TEST(TransformEditorVars, PythonRecipesRunThePythonChunk) {
  Rig rig;
  rig.newObjectRecipe("return {'count': cloud.count()}");
  rig.dialog().onToggled("pythonButton", true);
  rig.refresh();
  EXPECT_EQ(TransformEditorPreviewTestPeer::language(rig.editor), "python");
  const std::string script = TransformEditorPreviewTestPeer::script(rig.editor);
  EXPECT_NE(script.find("def evaluate(inputs, params):\n"), std::string::npos) << script;
  EXPECT_NE(script.find("    cloud = inputs[\"/cloud\"]\n"), std::string::npos) << script;
  EXPECT_NE(script.find("    return {'count': cloud.count()}"), std::string::npos) << script;
  ASSERT_GE(rig.dp.submits.size(), 1u);
  EXPECT_EQ(rig.dp.submits.back().language, "python") << "the trial runs the selected language";
  EXPECT_NE(rig.dp.submits.back().script.find("def evaluate"), std::string::npos);
  EXPECT_EQ(rig.dialog().canCreateReason(), "");
}

TEST(TransformEditorVars, ReturnArityReadsTheReturnStatements) {
  EXPECT_EQ(returnArity("return value*2", "luau"), 1u);
  EXPECT_EQ(returnArity("return a, b", "luau"), 2u);
  EXPECT_EQ(returnArity("return f(a, b), {1, 2}, \"x,y\"", "luau"), 3u);
  EXPECT_EQ(returnArity("if x then return nil end\nreturn a, b, c", "luau"), 3u);
  EXPECT_EQ(returnArity("-- return a, b\nreturn a", "luau"), 1u);
  EXPECT_EQ(returnArity("local s = 'return a, b'\nreturn a", "luau"), 1u);
  EXPECT_EQ(returnArity("return a,\n  b", "luau"), 2u);
  EXPECT_EQ(returnArity("x = 1", "luau"), 1u) << "no return: one";
  EXPECT_EQ(returnArity("return a, b", "python"), 2u);
  EXPECT_EQ(returnArity("return a  # x, y", "python"), 1u);
  EXPECT_EQ(returnArity("returned = 1, 2\nreturn z", "python"), 1u);
}

// ---------------------------------------------------------------------------
// The preview of the outputs
// ---------------------------------------------------------------------------

TEST(TransformEditorPreview, ObjectOutputsPreviewInAnEphemeralRecipeShownInTheEmbeddedSceneView) {
  Rig rig;
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  ASSERT_EQ(rig.dp.create_v2_calls, 1);
  EXPECT_EQ(
      rig.dp.last_create_v2_flags,
      static_cast<std::uint32_t>(PJ_DATA_PROCESSOR_FLAG_EPHEMERAL | PJ_DATA_PROCESSOR_FLAG_INFER_OUTPUTS));
  EXPECT_EQ(rig.dp.last_id, "__te_obj_preview__");
  EXPECT_EQ(rig.dp.last_outputs, (std::vector<std::string>{"cropped", "count"})) << "the inferred outputs";
  EXPECT_EQ(rig.dp.last_create_v2_time_flags, 0u) << "no instant: the preview follows the cursor";
  EXPECT_TRUE(rig.tabs.tabs.empty()) << "no scene tab: the objects show in the embedded view";
  EXPECT_EQ(rig.dp.liveCount(), 0) << "an ephemeral recipe is never one of the user's";
  const auto widgets = rig.widgets();
  EXPECT_EQ(widgets["frameScenePreview"]["scene_view"], "3d");
  ASSERT_EQ(widgets["frameScenePreview"]["scene_topics"].size(), 1u);
  EXPECT_EQ(widgets["frameScenePreview"]["scene_topics"][0]["topic"], "cropped");
  EXPECT_EQ(widgets["frameScenePreview"]["visible"], true);
  EXPECT_EQ(widgets["framePlotPreview"]["visible"], true) << "a mixed return shows both panes";

  // Ticks with an unchanged form install nothing new and keep sending the view.
  TransformEditorPreviewTestPeer::tick(rig.editor);
  EXPECT_EQ(rig.dp.create_v2_calls, 1);
  EXPECT_EQ(rig.widgets()["frameScenePreview"]["scene_view"], "3d");

  // A change re-upserts the same id in place (no remove) and the view keeps its topics.
  rig.dp.last_removed.clear();
  rig.dialog().onTextChanged("paramsLineEdit", "{\"k\":3}");
  rig.refresh();
  EXPECT_EQ(rig.dp.create_v2_calls, 2);
  EXPECT_TRUE(rig.dp.last_removed.empty());
  EXPECT_TRUE(rig.tabs.tabs.empty());
  const auto again = rig.widgets();
  ASSERT_EQ(again["frameScenePreview"]["scene_topics"].size(), 1u);
  EXPECT_NE(rig.dp.last_create_v2_params_json.find("\"k\":3"), std::string::npos);
}

TEST(TransformEditorPreview, ObjectsOnlyHideThePlotAndNumbersOnlyHideTheSceneAndClearTheView) {
  Rig rig;
  rig.dp.canned_report_json = kObjectsReport;
  rig.newObjectRecipe("return { cropped = cloud }");
  auto widgets = rig.widgets();
  EXPECT_EQ(widgets["frameScenePreview"]["scene_view"], "3d");
  EXPECT_EQ(widgets["frameScenePreview"]["visible"], true);
  EXPECT_EQ(widgets["framePlotPreview"]["visible"], false) << "objects only: the scene takes the area";
  EXPECT_TRUE(rig.tabs.tabs.empty());

  // The script now returns a number: the view goes away and the plot returns.
  rig.dp.canned_report_json = kNumberReport;
  rig.dialog().onCodeChanged("functionText", "return cloud:count()");
  rig.refresh();
  widgets = rig.widgets();
  EXPECT_EQ(widgets["frameScenePreview"]["visible"], false);
  EXPECT_EQ(widgets["framePlotPreview"]["visible"], true);
  ASSERT_TRUE(widgets["frameScenePreview"].contains("scene_view"));
  EXPECT_TRUE(widgets["frameScenePreview"]["scene_view"].is_null()) << "clearSceneView";
  EXPECT_TRUE(widgets["frameScenePreview"]["scene_topics"].empty());
  // The full state goes out every tick: the view stays cleared.
  TransformEditorPreviewTestPeer::tick(rig.editor);
  const auto later = rig.widgets();
  ASSERT_TRUE(later["frameScenePreview"].contains("scene_view"));
  EXPECT_TRUE(later["frameScenePreview"]["scene_view"].is_null());
}

TEST(TransformEditorPreview, WithoutTheHostsSceneViewBitTheSceneFrameStaysHiddenAndTheReadoutShows) {
  Rig rig;
  rig.embeds_scene_views = false;  // scene workspaces exist (tabs), but the dialog host binds no scene_view frame
  rig.rebind();
  rig.dp.canned_report_json = kTrialReport;
  rig.newObjectRecipe();
  const auto widgets = rig.widgets();
  EXPECT_EQ(widgets["frameScenePreview"]["visible"], false);
  EXPECT_TRUE(widgets["frameScenePreview"]["scene_view"].is_null());
  EXPECT_EQ(widgets["framePlotPreview"]["visible"], true);
  EXPECT_NE(TransformEditorPreviewTestPeer::status(rig.editor).find("count: 42"), std::string::npos);
  EXPECT_FALSE(rig.dialog().trialFailed());
}

TEST(TransformEditorPreview, AHostWithoutCatalogV2StillWarnsAboutObjectInputs) {
  Rig rig;
  rig.catalog.supportsV2(false);  // the v1 snapshot lists scalars only: an object topic is not there
  rig.catalog.addTopic("/imu").addField("/imu", "x");
  rig.rebind();
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/imu/x"});
  rig.dialog().onCodeChanged("functionText", "return value");
  rig.refresh();
  EXPECT_FALSE(rig.dialog().isOnDemand()) << "a scalar is a series input on any host";
  EXPECT_EQ(rig.dialog().canCreateReason().find("SDK 0.36"), std::string::npos);

  rig.dialog().onItemsDropped("tableSources", {"/cloud"});
  rig.refresh();
  EXPECT_TRUE(rig.dialog().isOnDemand()) << "not a scalar of the v1 snapshot: object-shaped";
  EXPECT_NE(rig.dialog().canCreateReason().find("SDK 0.36"), std::string::npos) << rig.dialog().canCreateReason();
  EXPECT_EQ(rig.widgets()["pushButtonCreate"]["enabled"], false);
  EXPECT_EQ(rig.dp.submit_calls, 0);
}

TEST(TransformEditorPreview, NumbersOnlySendAClearedSceneView) {
  Rig rig;
  rig.dp.canned_report_json = kNumberReport;
  rig.newObjectRecipe("return cloud:count()");
  const auto widgets = rig.widgets();
  ASSERT_TRUE(widgets["frameScenePreview"].contains("scene_view"));
  EXPECT_TRUE(widgets["frameScenePreview"]["scene_view"].is_null());
  EXPECT_EQ(widgets["frameScenePreview"]["visible"], false);
  EXPECT_EQ(widgets["framePlotPreview"]["visible"], true);
}

TEST(TransformEditorPreview, TheEmbeddedViewIs2DOnlyWhenEveryObjectOutputIs2D) {
  Rig rig;
  rig.dp.canned_report_json = kImageReport;
  rig.newObjectRecipe("return { img = cloud }");
  EXPECT_EQ(rig.widgets()["frameScenePreview"]["scene_view"], "2d");
  // A cloud next to the number of the default report is 3D.
  rig.dp.canned_report_json = kTrialReport;
  rig.dialog().onCodeChanged("functionText", "return { cropped = cloud, count = 1 }");
  rig.refresh();
  EXPECT_EQ(rig.widgets()["frameScenePreview"]["scene_view"], "3d");
}

TEST(TransformEditorPreview, CreateClearsTheEmbeddedView) {
  Rig rig;
  rig.dp.canned_report_json = kObjectsReport;
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  ASSERT_EQ(rig.widgets()["frameScenePreview"]["scene_view"], "3d");
  TransformEditorPreviewTestPeer::save(rig.editor);
  EXPECT_EQ(rig.dp.last_removed, "__te_obj_preview__");
  const auto widgets = rig.widgets();
  ASSERT_TRUE(widgets["frameScenePreview"].contains("scene_view"));
  EXPECT_TRUE(widgets["frameScenePreview"]["scene_view"].is_null());
}

TEST(TransformEditorPreview, NumberOutputsShowTheReadoutAndSayWhenTheSeriesNeedsANewerHost) {
  Rig rig;
  rig.dp.canned_report_json = kNumberReport;
  rig.newObjectRecipe("return cloud:count()");
  const auto widgets = rig.widgets();
  const std::string placeholder = widgets["framePlotPreview"]["chart_placeholder"].dump();
  EXPECT_NE(placeholder.find("value: 1.83"), std::string::npos) << placeholder;
  EXPECT_NE(placeholder.find("series preview needs a newer host"), std::string::npos) << placeholder;
  EXPECT_EQ(widgets["frameScenePreview"]["visible"], false);
  EXPECT_TRUE(rig.tabs.tabs.empty()) << "no object output: no scene tab";
  EXPECT_EQ(TransformEditorPreviewTestPeer::status(rig.editor), "value: 1.83");
  EXPECT_EQ(rig.dialog().canCreateReason(), "") << "a missing series preview never blocks Create";
}

TEST(TransformEditorPreview, CreateRemovesThePreviewRecipeAndCloseLeavesItToTheHost) {
  {
    Rig rig;
    rig.load(onDemandConfig("my_filter"));
    rig.refresh();
    ASSERT_EQ(rig.widgets()["frameScenePreview"]["scene_view"], "3d");
    rig.dp.last_removed.clear();
    TransformEditorPreviewTestPeer::save(rig.editor);
    EXPECT_EQ(rig.dp.last_removed, "__te_obj_preview__");
    EXPECT_EQ(rig.dp.liveCount(), 1);
  }
  RecordingDpHost dp;
  FakeCatalogHost catalog;
  FakePlaybackHost playback;
  FakePlotTabsHost tabs;
  catalog.addObjectTopic("/cloud", "kPointCloud", 5, 0, 5'000'000'000);
  dp.canned_report_json = kTrialReport;
  {
    TransformEditorToolbox editor;
    TransformEditorPreviewTestPeer::bind(editor, dp, catalog, playback, tabs);
    ASSERT_TRUE(TransformEditorPreviewTestPeer::dialog(editor).loadConfig(onDemandConfig("f")));
    TransformEditorPreviewTestPeer::refresh(editor);
    ASSERT_EQ(dp.create_v2_calls, 1);
  }  // closing the editor
  EXPECT_TRUE(dp.last_removed.empty()) << "the host removes the previews of a closed panel";
}

TEST(TransformEditorPreview, AFailedTrialTakesThePreviewRecipeAway) {
  Rig rig;
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  ASSERT_EQ(rig.widgets()["frameScenePreview"]["scene_view"], "3d");
  rig.dp.canned_report_json = R"({"error":"boom"})";
  rig.dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
  rig.dialog().onCodeChanged("functionText", "return nope");
  rig.refresh();
  EXPECT_EQ(rig.dp.last_removed, "__te_obj_preview__");
  EXPECT_TRUE(rig.widgets()["frameScenePreview"]["scene_view"].is_null());
}

TEST(TransformEditorPreview, SeriesOnlyRecipesInstallNoScenePreview) {
  Rig rig;
  rig.load(R"({"output_name":"dbl","global_code":"","function_body":"return value*2","sources":["a/x"]})");
  rig.refresh();
  EXPECT_EQ(rig.dp.create_v2_calls, 0);
  EXPECT_TRUE(rig.tabs.tabs.empty());
}

TEST(TransformEditorPreview, WithoutSceneTabsTheEmbeddedViewStillFollowsTheDialogHostBit) {
  Rig rig;
  rig.tabs.null_tail = true;
  rig.rebind();
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  EXPECT_TRUE(rig.tabs.tabs.empty());
  EXPECT_EQ(rig.widgets()["frameScenePreview"]["scene_view"], "3d") << "the bit alone gates the embedded view";
  EXPECT_NE(TransformEditorPreviewTestPeer::status(rig.editor).find("count: 42"), std::string::npos);
}

TEST(TransformEditorPreview, SummaryReadsAPointCloudReport) {
  const std::string report =
      R"({"bundles":[{"outputs":{"cloud":{"status":"ok","summary":{"type":"kPointCloud","points":23144,)"
      R"("bounds":{"min":[-10,-10,-2],"max":[10,10,3]}}},"count":{"status":"ok","value":42}}}]})";
  EXPECT_EQ(parseTrialReport(report).summary, "cloud: point cloud, 23 144 points\ncount: 42");
  EXPECT_EQ(parseTrialReport(R"({"error":"boom"})").summary, "boom");
  EXPECT_EQ(
      parseTrialReport(R"({"bundles":[{"outputs":{"o":{"status":"unavailable","reason":"no frame"}}}]})").summary,
      "o: unavailable (no frame)");
}

// ---------------------------------------------------------------------------
// The Function Library
// ---------------------------------------------------------------------------

TEST(TransformEditorLibrary, TheNewFieldsRoundTripAndAnOldLibraryStillLoads) {
  const auto dir = std::filesystem::temp_directory_path() / "te_snippets_test";
  std::filesystem::create_directories(dir);
  const auto path = dir / "snippets.json";

  Snippet object;
  object.name = "crop";
  object.function_body = "return cloud:count()";
  object.kind = "object";
  object.inputs = {{"cloud", "kPointCloud"}, {"info", "kCameraInfo"}};
  object.description = "counts";
  Snippet series;
  series.name = "twice";
  series.function_body = "return value*2";
  ASSERT_TRUE(saveSnippetsToPath({object, series}, path));
  const auto loaded = loadSnippetsFromPath(path);
  ASSERT_TRUE(loaded);
  ASSERT_EQ(loaded->size(), 2u);
  EXPECT_EQ((*loaded)[0].kind, "object");
  ASSERT_EQ((*loaded)[0].inputs.size(), 2u);
  EXPECT_EQ((*loaded)[0].inputs[1].var, "info");
  EXPECT_EQ((*loaded)[0].inputs[1].type, "kCameraInfo");
  EXPECT_EQ((*loaded)[0].description, "counts");
  EXPECT_EQ((*loaded)[1].kind, "series");
  EXPECT_TRUE((*loaded)[1].inputs.empty());

  // A library written by v1.0.x has none of the fields; a hand-edited one may have them malformed.
  {
    std::ofstream out(path);
    out << R"([{"name":"old","global_code":"g","function_body":"return value","language":"python"},)"
           R"({"name":"odd","function_body":"x","kind":7,"inputs":"nope","description":3},)"
           R"({"name":"mixed","function_body":"x","kind":"object","inputs":[{"var":"a","type":"kImage"},{"type":"x"},5]}])";
  }
  const auto old = loadSnippetsFromPath(path);
  ASSERT_TRUE(old);
  ASSERT_EQ(old->size(), 3u);
  EXPECT_EQ((*old)[0].name, "old");
  EXPECT_EQ((*old)[0].language, "python");
  EXPECT_EQ((*old)[0].kind, "series");
  EXPECT_TRUE((*old)[0].inputs.empty());
  EXPECT_EQ((*old)[0].description, "");
  EXPECT_EQ((*old)[1].kind, "series") << "a malformed kind falls back";
  EXPECT_TRUE((*old)[1].inputs.empty());
  EXPECT_EQ((*old)[2].inputs.size(), 1u) << "only the well-formed input is kept";
  std::filesystem::remove_all(dir);
}

TEST(TransformEditorLibrary, TheBuiltInsKeepTheNineSeriesFunctionsAndAddTheObjectOnes) {
  const auto snippets = defaultSnippets();
  const auto count = [&](const std::string& kind) {
    return std::count_if(snippets.begin(), snippets.end(), [&](const Snippet& s) { return s.kind == kind; });
  };
  EXPECT_EQ(count("series"), 9);
  EXPECT_EQ(count("object"), 7);
  for (const char* name :
       {"points_per_frame", "lidar_crop", "lidar_crop_map", "witness_of_crop", "cam_threshold", "cam_annotations",
        "depth_cloud"}) {
    const auto it = std::find_if(snippets.begin(), snippets.end(), [&](const Snippet& s) { return s.name == name; });
    ASSERT_NE(it, snippets.end()) << name;
    EXPECT_EQ(it->kind, "object");
    EXPECT_FALSE(it->inputs.empty()) << name;
    EXPECT_FALSE(it->description.empty()) << name;
    EXPECT_EQ(it->language, "luau");
    EXPECT_EQ(it->function_body.find("params."), std::string::npos) << name << " needs no params";
    EXPECT_EQ(it->function_body.find("inputs["), std::string::npos) << name << " reads its variables";
  }
  const auto depth =
      std::find_if(snippets.begin(), snippets.end(), [](const Snippet& s) { return s.name == "depth_cloud"; });
  ASSERT_EQ(depth->inputs.size(), 2u);
  EXPECT_EQ(depth->inputs[0].var, "depth");
  EXPECT_EQ(depth->inputs[1].var, "camera_info");
}

TEST(TransformEditorLibrary, UseOnAnObjectFunctionBindsTheVarOfTheFirstInputOfThatType) {
  Rig rig;
  rig.catalog.addObjectTopic("/img", "kImage", 1, 0, 1);
  rig.catalog.addObjectTopic("/depth", "kDepthImage", 1, 0, 1);
  rig.catalog.addObjectTopic("/info", "kCameraInfo", 1, 0, 1);
  rig.refresh();
  rig.dialog().setSnippets(defaultSnippets());
  rig.dialog().onItemsDropped("tableSources", {"/img", "/cloud", "/info", "/depth"});
  rig.dialog().loadSnippetsIntoEditor({"depth_cloud"});
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"img", "cloud", "camera_info", "depth"}));
  EXPECT_EQ(rig.dialog().language(), "luau");
  EXPECT_NE(rig.dialog().functionBody().find("depth:to_point_cloud(camera_info"), std::string::npos);
  EXPECT_EQ(TransformEditorPreviewTestPeer::status(rig.editor).find("needs:"), std::string::npos);

  rig.dialog().loadSnippetsIntoEditor({"lidar_crop"});
  EXPECT_EQ(rig.dialog().variableNames()[1], "cloud");
  EXPECT_EQ(rig.dialog().variableNames()[0], "img");
}

TEST(TransformEditorLibrary, UseWithNoInputOfTheNeededTypeSaysWhatIsNeeded) {
  Rig rig;
  rig.refresh();
  rig.dialog().setSnippets(defaultSnippets());
  rig.dialog().onItemsDropped("tableSources", {"a/x"});
  rig.dialog().loadSnippetsIntoEditor({"lidar_crop"});
  EXPECT_EQ(TransformEditorPreviewTestPeer::status(rig.editor), "needs: cloud (point cloud)");
  rig.dialog().loadSnippetsIntoEditor({"depth_cloud"});
  EXPECT_EQ(
      TransformEditorPreviewTestPeer::status(rig.editor), "needs: depth (depth image), camera_info (camera info)");
  rig.dialog().onItemsDropped("tableSources", {"/cloud"});  // editing the form drops the remark
  EXPECT_EQ(TransformEditorPreviewTestPeer::status(rig.editor).find("needs:"), std::string::npos);
}

TEST(TransformEditorLibrary, UseOnASeriesFunctionJustLoadsTheCode) {
  Rig rig;
  rig.refresh();
  rig.dialog().setSnippets(defaultSnippets());
  rig.dialog().onItemsDropped("tableSources", {"a/x"});
  rig.dialog().loadSnippetsIntoEditor({"rad_to_deg"});
  EXPECT_EQ(rig.dialog().functionBody(), "return value*180/3.14159");
  EXPECT_EQ(TransformEditorPreviewTestPeer::status(rig.editor).find("needs:"), std::string::npos);
}

TEST(TransformEditorLibrary, TheTableShowsAKindColumnAndThePreviewTheDescriptionAndInputs) {
  Rig rig;
  rig.dialog().setSnippets(defaultSnippets());
  rig.dialog().onClicked("buttonLibraryBox");
  auto widgets = rig.widgets();
  EXPECT_EQ(widgets["tableFunctions"]["headers"], nlohmann::json::array({"Function", "Kind", "Language"}));
  const auto rows = widgets["tableFunctions"]["rows"];
  const auto row = [&](const std::string& name) {
    for (const auto& r : rows) {
      if (r[0] == name) {
        return r;
      }
    }
    return nlohmann::json();
  };
  EXPECT_EQ(row("rad_to_deg")[1], "Series");
  EXPECT_EQ(row("lidar_crop")[1], "3D");
  EXPECT_EQ(row("cam_threshold")[1], "2D");
  EXPECT_EQ(row("depth_cloud")[1], "3D");
  rig.dialog().onSelectionChanged("tableFunctions", {"depth_cloud"});
  widgets = rig.widgets();
  const std::string info = widgets["previewInfoLabel"]["label"];
  EXPECT_NE(info.find("point cloud"), std::string::npos) << info;
  EXPECT_NE(info.find("Inputs: depth (depth image), camera_info (camera info)"), std::string::npos) << info;
}

TEST(TransformEditorLibrary, SavingAnObjectFunctionRecordsItsInputs) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/cloud", "pose/x"});
  rig.dialog().onCodeChanged("functionText", "return cloud:count()");
  const Snippet saved = rig.dialog().currentAsSnippet("mine");
  EXPECT_EQ(saved.kind, "object");
  ASSERT_EQ(saved.inputs.size(), 1u) << "only the object inputs are recorded";
  EXPECT_EQ(saved.inputs[0].var, "cloud");
  EXPECT_EQ(saved.inputs[0].type, "kPointCloud");
  EXPECT_EQ(saved.function_body, "return cloud:count()");
}
