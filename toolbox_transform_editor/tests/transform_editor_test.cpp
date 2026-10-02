// SPDX-License-Identifier: MPL-2.0
// The Transform Editor through the real toolbox and dialog, against the recording and fake hosts the
// assistant's tests use. The plugin source is compiled in ONCE, for this one executable: the trial run,
// Create / Modify / load of on-demand recipes, the asynchronous preview with its teardown, and the floor
// host (manifest.json floor_test) that predates the typed requests and catalog snapshot v2. Nothing here
// runs a script: the recording host proves WHAT the editor asked the host to run or install.
#include <gtest/gtest.h>

#include <filesystem>
#include <pj_plugins/testing/toolbox_test_store.hpp>

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
  static void bind(TransformEditorToolbox& editor, PJ::sdk::DataProcessorsHostView host) {
    editor.dp_view_ = host;
    editor.edit_debounce_ = std::chrono::milliseconds(0);  // no waiting for quiet in a test
  }
  // The form resolves inputs against the catalog (object topics first), so a config-driven
  // preview needs one that knows "/cloud".
  static void bindCatalog(TransformEditorToolbox& editor, toolbox_testing::FakeCatalogHost& catalog) {
    editor.test_catalog_host_ = PJ::sdk::ToolboxHostView(catalog.makeHost());
  }
  static void submitPreview(TransformEditorToolbox& editor, std::string script = "return {count=1}") {
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
  static bool pending(const TransformEditorToolbox& editor) {
    return editor.pending_preview_.has_value();
  }
  static void expire(TransformEditorToolbox& editor) {
    editor.preview_deadline_ = std::chrono::steady_clock::time_point{};
  }
  static void configure(TransformEditorToolbox& editor, bool source, bool body) {
    nlohmann::json config = {
        {"kind", "on_demand"},
        {"function_body", body ? "return {count=1}" : ""},
        {"sources", source ? nlohmann::json::array({"/cloud"}) : nlohmann::json::array()}};
    ASSERT_TRUE(editor.dialog_.loadConfig(config.dump()));
  }
  static void refreshCurrent(TransformEditorToolbox& editor) {
    editor.refreshPreview();
  }
  // The status line shows the reason Create is disabled once no result is left, so "cleared" means that neither
  // the result of the trial nor a pending run is on screen any more.
  static bool showsReport(TransformEditorToolbox& editor) {
    return status(editor).find("count: 42") != std::string::npos;
  }
  // The preview recipe's number output `name` was installed and the host answered `topic` for it; the
  // editor reads the series for it now.
  static void readSeries(
      TransformEditorToolbox& editor, PJ::testing::ToolboxTestStore& store, const std::string& name,
      const std::string& topic) {
    editor.test_catalog_host_ = PJ::sdk::ToolboxHostView(store.makeHost());
    editor.object_preview_topics_ = {topic};
    editor.showSeriesOrReadout({{name, "number"}}, TransformEditorToolbox::OnDemandBuild{});
  }
  // One read of the preview series as a tick does it, against `store` (the catalog the samples are read from);
  // the scene catalog the rig set is put back afterwards.
  static void pollSeries(
      TransformEditorToolbox& editor, PJ::testing::ToolboxTestStore& store,
      const std::vector<PJ::sdk::DataProcessorOutput>& outputs, const std::string& topic) {
    const auto previous = editor.test_catalog_host_;
    editor.test_catalog_host_ = PJ::sdk::ToolboxHostView(store.makeHost());
    editor.object_preview_topics_ = {topic};
    editor.next_series_read_ = {};
    editor.showSeriesOrReadout(outputs, editor.buildOnDemandRequest());
    editor.test_catalog_host_ = previous;
  }
  // The host accepted the preview recipe again (an edit re-upserts it): its series starts over.
  static void reinstall(TransformEditorToolbox& editor) {
    ++editor.object_preview_installs_;
  }
  static bool seriesAvailable(const TransformEditorToolbox& editor) {
    return editor.series_available_;
  }
  static std::string widgetText(TransformEditorToolbox& editor) {
    return editor.dialog_.widget_data();
  }
  static void close(TransformEditorToolbox& editor) {
    editor.tearDownPreview();
  }
  // A floor host: the v1 catalog lists scalars only and the data-processors vtable stops before the typed slots.
  struct FloorHost {
    RecordingDpHost dp;
    FakeCatalogHost catalog;
    FloorHost() {
      dp.supports_v2 = false;
      catalog.supportsV2(false);
      catalog.addTopic("/imu").addField("/imu", "x");
    }
  };

  static void bind(TransformEditorToolbox& editor, FloorHost& host) {
    editor.dp_view_ = host.dp.view();
    editor.test_catalog_host_ = PJ::sdk::ToolboxHostView(host.catalog.makeHost());
    editor.edit_debounce_ = std::chrono::milliseconds(0);
  }
  static void load(TransformEditorToolbox& editor, const std::string& config) {
    ASSERT_TRUE(editor.dialog_.loadConfig(config));
    editor.refreshPreview();
  }
  static void saveAsIs(TransformEditorToolbox& editor) {
    editor.onSave();
  }
  static std::string createReason(TransformEditorToolbox& editor) {
    return editor.dialog_.canCreateReason();
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
    catalog.addObjectTopic("/cloud", "kPointCloud", 5, 0, 5000000000);
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

using Host = RecordingDpHost;

// Double-click the Var of input row `row` and answer the prompt with `text` (OK). Returns the prompt's widgets.
nlohmann::json renameVar(Rig& rig, int row, const std::string& text, bool press_ok = true) {
  rig.dialog().onItemDoubleClicked("tableSources", row);
  auto widgets = rig.widgets();  // the prompt is a one-shot of this build
  rig.dialog().onTextChanged("renameVarName", text);
  if (press_ok) {
    rig.dialog().onClicked("subDialogAccepted");
  }
  return widgets;
}

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
  EXPECT_EQ(rig.dp.submits[0].instant_ns, 3000000000) << "at the cursor";
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

TEST(TransformEditorTrial, NoSampleAtTheCursorAfterTheFirstEntryStillRetriesAtTheFirstEntry) {
  Rig rig;
  rig.catalog.addObjectTopic("/late", "kPointCloud", 5, 8000000000, 9000000000);
  rig.playback.state.current_time_s = 8.5;
  rig.dp.report_queue = {kNoSampleReport};  // the first answer: no sample at the cursor; then the canned report
  rig.load(onDemandConfig("my_filter", "{}", false, "/late"));
  rig.refresh();
  ASSERT_EQ(rig.dp.submit_calls, 2);
  EXPECT_EQ(rig.dp.submits[0].instant_ns, 8500000000);
  EXPECT_EQ(rig.dp.submits[1].instant_ns, 8000000000) << "the first entry of /late";
  const std::string status = TransformEditorPreviewTestPeer::status(rig.editor);
  EXPECT_NE(status.find("No sample of /late at the cursor (8.500 s): move the timeline to preview."), std::string::npos)
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
  rig.catalog.addObjectTopic("/lidar_top", "kPointCloud", 5, 0, 5000000000);
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
  rig.catalog.addObjectTopic("/lidar_top", "kPointCloud", 5, 0, 5000000000);
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
  rig.catalog.addObjectTopic("/late", "kPointCloud", 5, 8000000000, 9000000000);
  rig.dp.canned_report_json = kNoSampleReport;
  rig.load(onDemandConfig("my_filter", "{}", false, "/late"));
  rig.refresh();
  EXPECT_EQ(rig.dp.submit_calls, 1) << "the cursor is before the first entry: straight there, no second try";
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
  EXPECT_EQ(rig.dp.submits[1].instant_ns, 4000000000);
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
  EXPECT_EQ(rig.dp.last_create_v2_instant_ns, 3000000000);  // playhead 3 s through the shared toRawNs
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
    Rig rig;  // a script error: the host's message, and the names the script can read
    rig.dp.canned_report_json = R"({"error":"attempt to index nil with 'crop_box'"})";
    rig.dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
    rig.load(onDemandConfig("my_filter"));
    rig.refresh();
    const std::string shown = std::string("attempt to index nil with 'crop_box' ") + kMiddleDot + " inputs are: cloud";
    EXPECT_EQ(rig.dialog().canCreateReason(), shown);
    EXPECT_EQ(TransformEditorPreviewTestPeer::status(rig.editor), shown);
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
  rig.catalog.addObjectTopic("/a", "kPointCloud", 1, 0, 1000000000, "{}", "runA");
  rig.catalog.addObjectTopic("/b", "kPointCloud", 1, 0, 1000000000, "{}", "runB");
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
  renameVar(rig, 0, "b_cloud");
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
  rig.catalog.addObjectTopic("/img", "kImage", 3, 0, 1000000000);
  rig.catalog.addObjectTopic("/ann", "kImageAnnotations", 3, 0, 1000000000);
  rig.catalog.addObjectTopic("/scene", "kSceneEntities", 3, 0, 1000000000);
  rig.catalog.addObjectTopic("/tf", "kFrameTransforms", 3, 0, 1000000000);
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
  rig.catalog.addObjectTopic("/img", "kImage", 3, 0, 1000000000);
  rig.catalog.addObjectTopic("/a", "kPointCloud", 1, 0, 1000000000, "{}", "runA");
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

TEST(TransformEditorPreview, NumberOutputsShowTheReadoutWhileTheSeriesIsComputing) {
  Rig rig;
  rig.dp.canned_report_json = kNumberReport;
  rig.dp.config_series = {{"value", 0, 0, "", false}};  // the host has not produced a row yet
  rig.newObjectRecipe("return cloud:count()");
  const auto widgets = rig.widgets();
  const std::string placeholder = widgets["framePlotPreview"]["chart_placeholder"].dump();
  EXPECT_NE(placeholder.find("value: 1.83"), std::string::npos) << placeholder;
  EXPECT_NE(placeholder.find("computing the series"), std::string::npos) << placeholder;
  EXPECT_EQ(placeholder.find("newer host"), std::string::npos) << "that note is gone: " << placeholder;
  EXPECT_EQ(widgets["frameScenePreview"]["visible"], false);
  EXPECT_TRUE(rig.tabs.tabs.empty()) << "no object output: no scene tab";
  EXPECT_EQ(
      TransformEditorPreviewTestPeer::status(rig.editor),
      std::string("value: 1.83 ") + kMiddleDot + " computing the series…");
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
  catalog.addObjectTopic("/cloud", "kPointCloud", 5, 0, 5000000000);
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

TEST(TransformEditorLibrary, UseOnAnObjectFunctionRewritesTheTextToTheVarsOfTheMatchingInputs) {
  Rig rig;
  rig.catalog.addObjectTopic("/img", "kImage", 1, 0, 1);
  rig.catalog.addObjectTopic("/depth", "kDepthImage", 1, 0, 1);
  rig.catalog.addObjectTopic("/info", "kCameraInfo", 1, 0, 1);
  rig.refresh();
  rig.dialog().setSnippets(defaultSnippets());
  rig.dialog().onItemsDropped("tableSources", {"/img", "/cloud", "/info", "/depth"});
  rig.dialog().loadSnippetsIntoEditor({"depth_cloud"});
  // The Vars stay what they were; the template's camera_info reads the Var of the camera info input.
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"img", "cloud", "info", "depth"}));
  EXPECT_EQ(rig.dialog().language(), "luau");
  EXPECT_NE(rig.dialog().functionBody().find("depth:to_point_cloud(info"), std::string::npos)
      << rig.dialog().functionBody();
  EXPECT_EQ(rig.dialog().functionBody().find("camera_info"), std::string::npos);
  EXPECT_EQ(TransformEditorPreviewTestPeer::status(rig.editor).find("needs:"), std::string::npos);

  rig.dialog().loadSnippetsIntoEditor({"lidar_crop"});
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"img", "cloud", "info", "depth"}));
  EXPECT_NE(rig.dialog().functionBody().find("cloud"), std::string::npos);
}

TEST(TransformEditorLibrary, UseReadsTheUsersOwnVarAndLeavesStringsFieldsAndKeysAlone) {
  Rig rig;
  rig.refresh();
  rig.dialog().setSnippets({Snippet{
      "crop",
      "",
      "return { cropped = cloud:crop(), note = \"cloud\", n = cloud.cloud }",
      "luau",
      "object",
      {{"cloud", "kPointCloud"}}}});
  rig.dialog().onItemsDropped("tableSources", {"/cloud"});
  renameVar(rig, 0, "lidar");
  rig.dialog().loadSnippetsIntoEditor({"crop"});
  EXPECT_EQ(rig.dialog().variableNames(), std::vector<std::string>{"lidar"}) << "the Var is the user's";
  EXPECT_EQ(rig.dialog().functionBody(), "return { cropped = lidar:crop(), note = \"cloud\", n = lidar.cloud }")
      << "only the variable reads the Var";
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

// ---------------------------------------------------------------------------
// The asynchronous preview and its teardown
// ---------------------------------------------------------------------------

TEST(TransformEditorPreview, PendingYieldsThenCompletesAndReleasesOnce) {
  Host host;
  host.pending_polls = 2;
  host.canned_report_json = kTrialReport;
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host.view());
  TransformEditorPreviewTestPeer::submitPreview(editor);
  EXPECT_EQ(host.poll_calls, 1);
  EXPECT_TRUE(TransformEditorPreviewTestPeer::pending(editor));
  EXPECT_TRUE(host.released_handles.empty());
  TransformEditorPreviewTestPeer::submitPreview(editor);
  TransformEditorPreviewTestPeer::submitPreview(editor);
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
  TransformEditorPreviewTestPeer::submitPreview(editor);
  TransformEditorPreviewTestPeer::submitPreview(editor);
  EXPECT_EQ(host.submit_calls, 1);
  TransformEditorPreviewTestPeer::releaseTrialTimer(editor);
  TransformEditorPreviewTestPeer::submitPreview(editor);
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
    TransformEditorPreviewTestPeer::submitPreview(editor);
    TransformEditorPreviewTestPeer::submitPreview(editor);
    EXPECT_FALSE(TransformEditorPreviewTestPeer::pending(editor));
    EXPECT_EQ(host.released_handles.size(), 1u);
  }
}
TEST(TransformEditorPreview, ExpiryReleasesOnceWithoutBlocking) {
  Host host;
  host.pending_polls = 100;
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host.view());
  TransformEditorPreviewTestPeer::submitPreview(editor);
  TransformEditorPreviewTestPeer::expire(editor);
  TransformEditorPreviewTestPeer::submitPreview(editor);
  EXPECT_FALSE(TransformEditorPreviewTestPeer::pending(editor));
  EXPECT_EQ(host.released_handles.size(), 1u);
}
TEST(TransformEditorPreview, ReplacementAndDestructionReleasePendingRequests) {
  Host host;
  host.pending_polls = 100;
  {
    TransformEditorToolbox editor;
    TransformEditorPreviewTestPeer::bind(editor, host.view());
    TransformEditorPreviewTestPeer::submitPreview(editor);
    TransformEditorPreviewTestPeer::submitPreview(editor, "return {count=2}");
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
  TransformEditorPreviewTestPeer::submitPreview(editor);
  ASSERT_TRUE(TransformEditorPreviewTestPeer::pending(editor));
  TransformEditorPreviewTestPeer::setDebounce(editor, std::chrono::seconds(30));  // the user keeps typing
  TransformEditorPreviewTestPeer::submitPreview(editor, "return {count=2}");
  EXPECT_FALSE(TransformEditorPreviewTestPeer::pending(editor)) << "the stale run is released at once";
  EXPECT_EQ(host.released_handles.size(), 1u);
  EXPECT_EQ(host.submit_calls, 1) << "nothing is submitted while the edits keep coming";
  TransformEditorPreviewTestPeer::setDebounce(editor, std::chrono::milliseconds(0));
  TransformEditorPreviewTestPeer::submitPreview(editor, "return {count=2}");
  EXPECT_EQ(host.submit_calls, 2);
}

TEST(TransformEditorPreview, TheSeriesPreviewReadsExactlyTheNameTheHostReturned) {
  const std::string real = "toolbox-transform-editor/__te_obj_preview__/value";
  // The real series: topic `<owner>/<id>`, one column per output. Decoys: a topic whose column leaf is also
  // `value`, one at the name the editor used to guess, and one with the same leaf under another topic.
  auto build = [&](bool with_real) {
    auto store = std::make_unique<PJ::testing::ToolboxTestStore>();
    if (with_real) {
      store->addTopic("toolbox-transform-editor/__te_obj_preview__")
          .addField("toolbox-transform-editor/__te_obj_preview__", "value", {0, 1000000000}, {1.0, 2.0});
    }
    store->addTopic("__te_obj_preview__").addField("__te_obj_preview__", "value", {0, 1000000000}, {9.0, 9.0});
    store->addTopic("/imu").addField("/imu", "value", {0, 1000000000}, {7.0, 7.0});
    return store;
  };
  {
    auto store = build(/*with_real=*/true);
    TransformEditorToolbox editor;
    TransformEditorPreviewTestPeer::readSeries(editor, *store, "value", real);
    ASSERT_TRUE(TransformEditorPreviewTestPeer::seriesAvailable(editor));
    const std::string shown = TransformEditorPreviewTestPeer::widgetText(editor);
    EXPECT_EQ(shown.find("9.0"), std::string::npos) << shown;
    EXPECT_EQ(shown.find("7.0"), std::string::npos) << shown;
  }
  {
    // The host did not materialize a series for this recipe (pinned, or no object input): nothing to plot,
    // whatever else is called `value` in the catalog.
    auto store = build(/*with_real=*/false);
    TransformEditorToolbox editor;
    TransformEditorPreviewTestPeer::readSeries(editor, *store, "value", real);
    EXPECT_FALSE(TransformEditorPreviewTestPeer::seriesAvailable(editor));
  }
  {
    // A host that returned no name for the output: the editor does not look one up.
    auto store = build(/*with_real=*/false);
    TransformEditorToolbox editor;
    TransformEditorPreviewTestPeer::readSeries(editor, *store, "value", "");
    EXPECT_FALSE(TransformEditorPreviewTestPeer::seriesAvailable(editor));
  }
}

TEST(TransformEditorPreview, TheHostsCoverageErrorIsShownNotANoSampleNote) {
  Host host;
  host.canned_report_json =
      R"({"coverage":{"complete":false,"stopped":"error","error":"input 'cloud' has no data source"},"bundles":[]})";
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host.view());
  TransformEditorPreviewTestPeer::submitPreview(editor);
  const std::string shown = TransformEditorPreviewTestPeer::widgetText(editor);
  EXPECT_NE(shown.find("input 'cloud' has no data source"), std::string::npos) << shown;
  EXPECT_EQ(shown.find("No sample to run on"), std::string::npos) << shown;
}

TEST(TransformEditorPreview, TheStatusLineShowsTheResultAndCreateWaitsForAnInferredTrial) {
  TransformEditorDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({"kind":"on_demand","function_body":"return {count=1}","sources":["/cloud"]})"));
  dialog.setStatus("count: 42");
  const auto widgets = nlohmann::json::parse(dialog.widget_data());
  EXPECT_EQ(widgets["statusLabel"]["label"], "count: 42");
  EXPECT_EQ(widgets["pushButtonCreate"]["enabled"], false) << "no trial has succeeded yet";
}

TEST(TransformEditorPreview, ClearingBodyOrInputClearsCompletedAndPendingReports) {
  for (bool clear_source : {false, true}) {
    Host host;
    host.canned_report_json = kTrialReport;
    toolbox_testing::FakeCatalogHost catalog;
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

// ---------------------------------------------------------------------------
// The preview series while the host replays it
// ---------------------------------------------------------------------------

namespace {

// The 1-based line of the first line of `script` that is exactly `line`.
int scriptLineOf(const std::string& script, const std::string& line) {
  std::istringstream in(script);
  std::string text;
  for (int n = 1; std::getline(in, text); ++n) {
    if (text == line) {
      return n;
    }
  }
  return -1;
}

const std::vector<PJ::sdk::DataProcessorOutput> kValueOutput = {{"value", "number"}};

// The points of the first plotted series of the dialog, -1 when nothing is plotted.
int plottedPoints(Rig& rig) {
  const auto series = rig.widgets()["framePlotPreview"]["chart_series"];
  return series.is_array() && !series.empty() ? static_cast<int>(series[0]["points"].size()) : -1;
}

std::string statusOf(Rig& rig) {
  return TransformEditorPreviewTestPeer::status(rig.editor);
}

// A rig whose recipe has one number output, `value`, plotted from the series topic "value" of `store`.
void numberRecipe(Rig& rig) {
  rig.dp.canned_report_json = kNumberReport;
  rig.newObjectRecipe("return cloud:count()");
  rig.dp.config_calls = 0;
  rig.dp.config_series_served = 0;
}

}  // namespace

TEST(TransformEditorSeries, ItIsReadAgainAsTheRowsGrowAndNotAnyMoreOnceComplete) {
  Rig rig;
  numberRecipe(rig);
  rig.dp.config_series = {{"value", 0, 0, "", false}, {"value", 4, 0, "", false}, {"value", 8, 0, "", true}};
  PJ::testing::ToolboxTestStore store;
  store.addTopic("value").addField("value", "v", {}, {});
  TransformEditorPreviewTestPeer::reinstall(rig.editor);

  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, kValueOutput, "value");
  EXPECT_EQ(plottedPoints(rig), -1) << "no row yet";
  EXPECT_NE(statusOf(rig).find("computing the series…"), std::string::npos) << statusOf(rig);

  store.extendField("v", {0, 1000000000, 2000000000, 3000000000}, {1, 2, 3, 4});
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, kValueOutput, "value");
  EXPECT_EQ(plottedPoints(rig), 4);
  EXPECT_NE(statusOf(rig).find("computing the series… 4 rows"), std::string::npos) << statusOf(rig);

  store.extendField("v", {4000000000, 5000000000, 6000000000, 7000000000}, {5, 6, 7, 8});
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, kValueOutput, "value");
  EXPECT_EQ(plottedPoints(rig), 8) << "re-read as the rows grew";
  EXPECT_EQ(statusOf(rig).find("computing"), std::string::npos) << statusOf(rig);
  EXPECT_EQ(rig.dp.last_config_id, "__te_obj_preview__") << "the owner reads its own ephemeral recipe by id";
  ASSERT_EQ(rig.dp.config_calls, 3);

  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, kValueOutput, "value");
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, kValueOutput, "value");
  EXPECT_EQ(rig.dp.config_calls, 3) << "complete: nothing is asked again";
  EXPECT_EQ(plottedPoints(rig), 8);
}

TEST(TransformEditorSeries, ARowCountThatDidNotMoveIsNotReadAgain) {
  Rig rig;
  numberRecipe(rig);
  rig.dp.config_series = {{"value", 4, 0, "", false}, {"value", 4, 0, "", false}, {"value", 6, 0, "", false}};
  PJ::testing::ToolboxTestStore store;
  store.addTopic("value").addField("value", "v", {0, 1000000000, 2000000000, 3000000000}, {1, 2, 3, 4});
  TransformEditorPreviewTestPeer::reinstall(rig.editor);
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, kValueOutput, "value");
  ASSERT_EQ(plottedPoints(rig), 4);
  // The store grows behind the editor's back: with the host's row count unchanged there is nothing new to read.
  store.extendField("v", {4000000000, 5000000000}, {5, 6});
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, kValueOutput, "value");
  EXPECT_EQ(plottedPoints(rig), 4);
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, kValueOutput, "value");
  EXPECT_EQ(plottedPoints(rig), 6) << "the row count moved: read again";
}

TEST(TransformEditorSeries, WhilePendingThePreviousCurveStaysAndTheStatusSaysComputing) {
  Rig rig;
  numberRecipe(rig);
  rig.dp.config_series = {{"value", 3, 0, "", true}};
  PJ::testing::ToolboxTestStore first;
  first.addTopic("value").addField("value", "v", {0, 1000000000, 2000000000}, {1, 2, 3});
  TransformEditorPreviewTestPeer::reinstall(rig.editor);
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, first, kValueOutput, "value");
  ASSERT_EQ(plottedPoints(rig), 3);

  // An edit re-installs the recipe: the host restarts the series, so the topic is empty for a while.
  PJ::testing::ToolboxTestStore restarted;
  restarted.addTopic("value").addField("value", "v", {}, {});
  rig.dp.config_series = {{"value", 0, 0, "", false}};
  rig.dp.config_series_served = 0;
  TransformEditorPreviewTestPeer::reinstall(rig.editor);
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, restarted, kValueOutput, "value");
  EXPECT_EQ(plottedPoints(rig), 3) << "the previous curve stays";
  EXPECT_NE(statusOf(rig).find("computing the series…"), std::string::npos) << statusOf(rig);
  EXPECT_EQ(statusOf(rig).find("newer host"), std::string::npos);
  EXPECT_EQ(rig.widgets()["framePlotPreview"]["chart_placeholder"], "") << "the curve is not covered by a note";

  // The same wait for outputs that are not the ones plotted: that curve is of no use any more.
  rig.dp.config_series_served = 0;
  TransformEditorPreviewTestPeer::reinstall(rig.editor);
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, restarted, {{"other", "number"}}, "value");
  EXPECT_EQ(plottedPoints(rig), -1) << "different names: cleared";

  // Rows arrive: the new curve replaces it.
  PJ::testing::ToolboxTestStore arrived;
  arrived.addTopic("value").addField("value", "v", {0, 1000000000}, {7, 8});
  rig.dp.config_series = {{"value", 2, 0, "", true}};
  rig.dp.config_series_served = 0;
  TransformEditorPreviewTestPeer::reinstall(rig.editor);
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, arrived, kValueOutput, "value");
  EXPECT_EQ(plottedPoints(rig), 2);
  EXPECT_EQ(statusOf(rig).find("computing"), std::string::npos) << statusOf(rig);
}

TEST(TransformEditorSeries, TheHostsFirstSeriesErrorIsOnTheStatusLine) {
  Rig rig;
  numberRecipe(rig);
  const int body_line = scriptLineOf(TransformEditorPreviewTestPeer::script(rig.editor), "return cloud:count()");
  ASSERT_GT(body_line, 1);
  rig.dp.config_series = {
      {"value", 0, 5, "script:" + std::to_string(body_line) + ": attempt to index nil with 'count'", true}};
  PJ::testing::ToolboxTestStore store;
  store.addTopic("value").addField("value", "v", {}, {});
  TransformEditorPreviewTestPeer::reinstall(rig.editor);
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, kValueOutput, "value");
  EXPECT_NE(statusOf(rig).find("series error: line 1: attempt to index nil with 'count'"), std::string::npos)
      << statusOf(rig) << " (the chunk line is the user's line)";
}

// ---------------------------------------------------------------------------
// A cursor before the first sample, errors that survive, errors in the user's own lines
// ---------------------------------------------------------------------------

TEST(TransformEditorTrial, ACursorBeforeTheFirstEntryRunsThereAndSaysSoInDisplaySeconds) {
  Rig rig;
  rig.playback.display_offset_ns = 1000000000000;  // display 0 s is raw 1000 s
  rig.playback.state.current_time_s = 3.0;
  rig.catalog.addObjectTopic("/late", "kPointCloud", 5, 1008000000000, 1009000000000);
  rig.dp.first_sample_ns = 1008000000000;  // the host cannot run an input before its first entry
  rig.dp.first_sample_input = "/late";
  rig.load(onDemandConfig("my_filter", "{}", false, "/late"));
  rig.refresh();
  ASSERT_EQ(rig.dp.submit_calls, 1) << "straight at the first entry: no run at the cursor";
  EXPECT_EQ(rig.dp.submits[0].instant_ns, 1008000000000);
  const std::string status = TransformEditorPreviewTestPeer::status(rig.editor);
  EXPECT_NE(
      status.find("No sample of /late at the cursor (3.000 s): move the timeline to preview. Outputs: "),
      std::string::npos)
      << status;
  EXPECT_EQ(status.find("t="), std::string::npos) << status;
  EXPECT_EQ(status.find("1003"), std::string::npos) << "no raw ns: " << status;
  EXPECT_EQ(status.find("missing input"), std::string::npos) << status;
  EXPECT_EQ(rig.dialog().canCreateReason(), "") << "the outputs are known: Create stays enabled";
  EXPECT_EQ(rig.widgets()["pushButtonCreate"]["enabled"], true);
  const std::string overlay = rig.widgets()["framePlotPreview"]["chart_placeholder"].dump();
  EXPECT_EQ(overlay.find("missing input"), std::string::npos) << overlay;
}

TEST(TransformEditorTrial, TheFirstEntryIsTheLatestFirstEntryOfTheInputs) {
  Rig rig;
  rig.catalog.addObjectTopic("/early", "kPointCloud", 5, 1000000000, 9000000000);
  rig.catalog.addObjectTopic("/late", "kPointCloud", 5, 4000000000, 9000000000);
  rig.playback.state.current_time_s = 2.0;
  rig.dp.first_sample_ns = 4000000000;
  rig.dp.first_sample_input = "/late";
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/early", "/late"});
  rig.dialog().onCodeChanged("functionText", "return { count = 1 }");
  rig.refresh();
  ASSERT_GE(rig.dp.submit_calls, 1);
  EXPECT_EQ(rig.dp.submits.back().instant_ns, 4000000000) << "the instant at which every input has a sample";
  const std::string status = TransformEditorPreviewTestPeer::status(rig.editor);
  EXPECT_NE(status.find("No sample of /late at the cursor (2.000 s)"), std::string::npos) << status;
}

TEST(TransformEditorTrial, TheValidationErrorSurvivesTheTickThatSubmitsAgain) {
  Rig rig;
  rig.dp.canned_report_json = R"({"error":"boom"})";
  rig.dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
  rig.load(onDemandConfig("my_filter"));
  rig.refresh();
  auto overlay = [&] { return rig.widgets()["framePlotPreview"]["chart_placeholder"].dump(); };
  ASSERT_NE(overlay().find("boom"), std::string::npos) << overlay();
  // The next trial is submitted and stays pending for a while: the verdict on screen is the last one.
  rig.dp.pending_polls = 100;
  TransformEditorPreviewTestPeer::releaseTrialTimer(rig.editor);
  TransformEditorPreviewTestPeer::tick(rig.editor);
  ASSERT_TRUE(TransformEditorPreviewTestPeer::pending(rig.editor));
  EXPECT_NE(overlay().find("boom"), std::string::npos) << "tick 1 (the submit): " << overlay();
  TransformEditorPreviewTestPeer::tick(rig.editor);
  EXPECT_NE(overlay().find("boom"), std::string::npos) << "tick 2 (still pending): " << overlay();
  // The next verdict replaces it.
  rig.dp.pending_polls = 0;
  rig.dp.canned_report_json = kTrialReport;
  rig.dp.terminal_state = PJ_EVALUATION_STATE_COMPLETED;
  TransformEditorPreviewTestPeer::expire(rig.editor);
  TransformEditorPreviewTestPeer::tick(rig.editor);
  rig.refresh();
  EXPECT_EQ(overlay().find("boom"), std::string::npos) << overlay();
}

TEST(TransformEditorTrial, AnErrorInTheUsersCodeNamesTheUsersLineAndTheInputs) {
  Rig rig;
  rig.catalog.addObjectTopic("/lidar_top", "kPointCloud", 5, 0, 5000000000);
  rig.dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
  rig.dp.canned_report_json = R"({"error":"x"})";
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/lidar_top"});
  rig.dialog().onCodeChanged("functionText", "return cloud.count");
  rig.refresh();
  // The host reports the line of the chunk; the chunk's directive and bindings come before the user's code.
  const std::string script = TransformEditorPreviewTestPeer::script(rig.editor);
  const int body_line = scriptLineOf(script, "return cloud.count");
  ASSERT_GT(body_line, 1);
  rig.dp.canned_report_json =
      R"({"error":"script:)" + std::to_string(body_line) + R"(: attempt to index nil with 'count'"})";
  rig.refresh();
  const std::string reason = rig.dialog().canCreateReason();
  EXPECT_EQ(reason, "line 1: attempt to index nil with 'count' " + std::string(kMiddleDot) + " inputs are: lidar_top");
  EXPECT_EQ(reason.find("script:"), std::string::npos) << reason;
  EXPECT_EQ(script.rfind("-- pj-script: luau\n", 0), 0u) << "the directive stays on line 1";

  // A second line of the body is the second line of the user's code.
  rig.dialog().onCodeChanged("functionText", "local n = 1\nreturn cloud.count");
  rig.refresh();
  const int second = scriptLineOf(TransformEditorPreviewTestPeer::script(rig.editor), "return cloud.count");
  rig.dp.canned_report_json =
      R"({"error":"script:)" + std::to_string(second) + R"(: attempt to index nil with 'count'"})";
  rig.refresh();
  EXPECT_EQ(rig.dialog().canCreateReason().rfind("line 2: attempt to index nil", 0), 0u);
}

TEST(TransformEditorTrial, APythonErrorIsRemappedToo) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/cloud"});
  rig.dialog().onCodeChanged("functionText", "return {'count': cloud.count()}");
  rig.dialog().onToggled("pythonButton", true);
  rig.dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
  rig.dp.canned_report_json = R"({"error":"x"})";
  rig.refresh();
  const int line =
      scriptLineOf(TransformEditorPreviewTestPeer::script(rig.editor), "    return {'count': cloud.count()}");
  ASSERT_GT(line, 2);
  rig.dp.canned_report_json = R"j({"error":"Python error: NameError: name 'cloudd' is not defined <string>()j" +
                              std::to_string(line) + R"j()"})j";
  rig.refresh();
  EXPECT_EQ(
      rig.dialog().canCreateReason(),
      "Python error: NameError: name 'cloudd' is not defined line 1 " + std::string(kMiddleDot) + " inputs are: cloud");
}

TEST(TransformEditorTrial, ASeriesFunctionErrorIsRemappedToTheUsersLine) {
  Rig rig;
  rig.dp.fail_validate = true;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"a/x"});
  rig.dialog().onCodeChanged("functionText", "local y = value\nreturn yy");
  rig.refresh();
  // The host names the line of the generated chunk; the user's code starts after the wrapper.
  const int line = scriptLineOf(rig.dp.last_validate_script, "return yy");
  ASSERT_GT(line, 3);
  rig.dp.validate_error = "script:" + std::to_string(line) + ": unknown global 'yy'";
  rig.dialog().onCodeChanged("functionText", "local y = value\nreturn yy ");  // a new form revision: asked again
  rig.refresh();
  EXPECT_EQ(rig.dialog().canCreateReason(), "test: line 2: unknown global 'yy'");
}

// ---------------------------------------------------------------------------
// The preview's zoom, and renaming a Var
// ---------------------------------------------------------------------------

// The user zooms and pans the preview and double-clicks it to fit (host side): the editor never sends
// chart_auto_zoom, which would wipe the user's view on every tick, and has no Fit button.
TEST(TransformEditorChart, TheEditorNeverSendsAutoZoom) {
  Rig rig;
  rig.dp.canned_report_json = kNumberReport;
  rig.newObjectRecipe("return cloud:count()");
  rig.dp.config_series = {{"value", 3, 0, "", true}};
  PJ::testing::ToolboxTestStore store;
  store.addTopic("value").addField("value", "v", {0, 1000000000, 2000000000}, {1, 2, 3});
  TransformEditorPreviewTestPeer::reinstall(rig.editor);
  TransformEditorPreviewTestPeer::pollSeries(rig.editor, store, {{"value", "number"}}, "value");
  ASSERT_EQ(plottedPoints(rig), 3);
  for (int tick = 0; tick < 3; ++tick) {
    const auto widgets = rig.widgets();
    EXPECT_FALSE(widgets["framePlotPreview"].contains("chart_auto_zoom")) << "tick " << tick;
    EXPECT_FALSE(widgets.contains("buttonFitPlot")) << "tick " << tick << ": the Fit button is gone";
  }
}

TEST(TransformEditorVars, ADoubleClickOnAnInputRowOpensThePromptWithTheCurrentVar) {
  Rig rig;
  rig.newObjectRecipe();
  const auto widgets = renameVar(rig, 0, "cloud", /*press_ok=*/false);
  ASSERT_TRUE(widgets.contains("__request_sub_dialog"));
  EXPECT_NE(std::string(widgets["__request_sub_dialog"]["ui"]).find("renameVarName"), std::string::npos);
  EXPECT_EQ(widgets["renameVarName"]["text"], "cloud");
  EXPECT_EQ(widgets["renameVarLabel"]["label"], "Variable name for /cloud:");
  EXPECT_EQ(widgets["renameVarNote"]["label"], "");
}

TEST(TransformEditorVars, RenamingSetsTheVarWithoutTouchingTheScriptAndItIsSaved) {
  Rig rig;
  rig.newObjectRecipe("return { cropped = cloud, count = 1 }");
  const std::string body = rig.dialog().functionBody();
  const std::string globals = rig.dialog().globalCode();
  renameVar(rig, 0, "lidar");
  EXPECT_EQ(rig.dialog().variableNames(), std::vector<std::string>{"lidar"});
  EXPECT_EQ(rig.dialog().functionBody(), body) << "the script is never edited by a rename";
  EXPECT_EQ(rig.dialog().globalCode(), globals);
  rig.refresh();
  EXPECT_NE(
      TransformEditorPreviewTestPeer::script(rig.editor).find("local lidar = inputs[\"/cloud\"]"), std::string::npos);
  EXPECT_EQ(rig.widgets()["tableSources"]["rows"][0][2], "lidar");
  EXPECT_EQ(nlohmann::json::parse(rig.dialog().saveConfig()).at("vars"), nlohmann::json::array({"lidar"}));

  Rig other;
  other.load(rig.dialog().saveConfig());
  other.refresh();
  EXPECT_EQ(other.dialog().variableNames(), std::vector<std::string>{"lidar"});
  EXPECT_EQ(other.dialog().functionBody(), body);
}

TEST(TransformEditorVars, ABadNameIsRefusedWithItsReasonAndThePromptReopens) {
  Rig rig;
  rig.catalog.addObjectTopic("/lidar_top", "kPointCloud", 1, 0, 1);
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/lidar_top", "/cloud"});
  ASSERT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"lidar_top", "cloud"}));
  struct Case {
    std::string name;
    std::string reason;
  };
  for (const Case& bad :
       {Case{"end", "reserved"}, Case{"inputs", "reserved"}, Case{"2x", "not a valid name"},
        Case{"a-b", "not a valid name"}, Case{"cloud", "already the name of another input"}}) {
    renameVar(rig, 0, bad.name);
    const auto widgets = rig.widgets();
    ASSERT_TRUE(widgets.contains("__request_sub_dialog")) << bad.name << ": the prompt reopens";
    EXPECT_NE(std::string(widgets["renameVarNote"]["label"]).find(bad.reason), std::string::npos)
        << bad.name << ": " << widgets["renameVarNote"]["label"];
    EXPECT_EQ(widgets["renameVarName"]["text"], bad.name) << "what was typed stays to be fixed";
    EXPECT_EQ(rig.dialog().variableNames()[0], "lidar_top") << bad.name << " changed nothing";
  }
  // A valid one on the reopened prompt is accepted.
  rig.dialog().onTextChanged("renameVarName", "crop");
  rig.dialog().onClicked("subDialogAccepted");
  EXPECT_EQ(rig.dialog().variableNames(), (std::vector<std::string>{"crop", "cloud"}));
}

TEST(TransformEditorVars, AnEmptyNameGivesBackTheDefaultAndTheOwnNameIsAccepted) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"/cloud"});
  renameVar(rig, 0, "crop");
  ASSERT_EQ(rig.dialog().variableNames(), std::vector<std::string>{"crop"});
  renameVar(rig, 0, "  ");
  EXPECT_EQ(rig.dialog().variableNames(), std::vector<std::string>{"cloud"}) << "the default of the row";
  EXPECT_EQ(nlohmann::json::parse(rig.dialog().saveConfig()).at("vars"), nlohmann::json::array({"cloud"}));
  renameVar(rig, 0, "cloud");  // the current name is not a clash with itself
  EXPECT_FALSE(rig.widgets().contains("__request_sub_dialog"));
  EXPECT_EQ(rig.dialog().variableNames(), std::vector<std::string>{"cloud"});
}

TEST(TransformEditorVars, OnlyOnDemandRecipesRenameAndABadRowIsIgnored) {
  Rig rig;
  rig.refresh();
  rig.dialog().onItemsDropped("tableSources", {"a/x", "b/y"});
  EXPECT_FALSE(rig.dialog().onItemDoubleClicked("tableSources", 0)) << "a series recipe keeps value and v1";
  EXPECT_FALSE(rig.widgets().contains("__request_sub_dialog"));
  Rig objects;
  objects.refresh();
  objects.dialog().onItemsDropped("tableSources", {"/cloud"});
  EXPECT_FALSE(objects.dialog().onItemDoubleClicked("tableSources", 5));
  EXPECT_FALSE(objects.dialog().onItemDoubleClicked("tableSources", -1));
  EXPECT_TRUE(objects.dialog().onItemDoubleClicked("tableSources", 0));
}

TEST(TransformEditorVars, AnAbandonedRenamePromptDoesNotHijackTheCreatePrompt) {
  Rig rig;
  rig.newObjectRecipe();
  rig.dialog().onItemDoubleClicked("tableSources", 0);
  (void)rig.widgets();
  // Cancel is not reported to the plugin: opening the Create prompt afterwards must not be taken for a rename.
  rig.dialog().onClicked("pushButtonCreate");
  (void)rig.widgets();
  rig.dialog().onTextChanged("createRecipeName", "made");
  rig.dialog().onClicked("subDialogAccepted");
  TransformEditorPreviewTestPeer::deliver(rig.editor);
  ASSERT_EQ(rig.dp.persistent_creates, 1);
  EXPECT_EQ(rig.dp.created[0].id, "made");
  EXPECT_EQ(rig.dialog().variableNames(), std::vector<std::string>{"cloud"});
}

// ---------------------------------------------------------------------------
// Floor host: no typed requests, no catalog v2
// ---------------------------------------------------------------------------

TEST(TransformEditorFloorTest, TheSeriesPathStillCreatesATransformOnAFloorHost) {
  TransformEditorPreviewTestPeer::FloorHost host;
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host);
  TransformEditorPreviewTestPeer::load(
      editor, R"({"output_name":"dbl","function_body":"return value*2","sources":["/imu/x"]})");
  EXPECT_EQ(TransformEditorPreviewTestPeer::createReason(editor), "");
  TransformEditorPreviewTestPeer::saveAsIs(editor);
  ASSERT_EQ(host.dp.persistent_creates, 1);
  EXPECT_EQ(host.dp.created[0].kind, "transform");
  EXPECT_EQ(host.dp.created[0].inputs, std::vector<std::string>{"/imu/x"});
  EXPECT_EQ(host.dp.submit_calls, 0);
}

TEST(TransformEditorFloorTest, OnDemandSubmitReportsUnsupportedOnFloorHost) {
  TransformEditorPreviewTestPeer::FloorHost host;
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host);
  // "/cloud" is not a scalar of the v1 snapshot: the editor cannot tell what it is on this host, so it
  // takes it for an object.
  TransformEditorPreviewTestPeer::load(
      editor, R"({"output_name":"n","function_body":"return {count = 1}","sources":["/cloud"]})");
  const std::string reason = TransformEditorPreviewTestPeer::createReason(editor);
  EXPECT_NE(reason.find("SDK 0.36"), std::string::npos) << reason;
  TransformEditorPreviewTestPeer::saveAsIs(editor);
  EXPECT_EQ(host.dp.persistent_creates, 0);
  EXPECT_EQ(host.dp.create_v2_calls, 0);
  EXPECT_EQ(host.dp.submit_calls, 0) << "no trial on a host without the typed requests";
}
