// SPDX-License-Identifier: MPL-2.0
// Create / Modify / load of on-demand recipes through the real toolbox and dialog (the plugin
// source is compiled in, like async_preview_test), against the recording and fake hosts the
// assistant's tests use. Nothing here runs a script: the recording host proves WHAT the editor
// asked the host to install, which is the contract this feature adds.
#include <gtest/gtest.h>

#include "../../toolbox_assistant_agent/tests/support/fake_catalog_host.hpp"
#include "../../toolbox_assistant_agent/tests/support/fake_playback_viewport_hosts.hpp"
#include "../../toolbox_assistant_agent/tests/support/fake_plot_tabs_host.hpp"
#include "../../toolbox_assistant_agent/tests/support/recording_dp_host.hpp"
#include "../transform_editor_plugin.cpp"

namespace {

using assistant_agent::testing::FakeCatalogHost;
using assistant_agent::testing::FakePlaybackHost;
using assistant_agent::testing::FakePlotTabsHost;
using assistant_agent::testing::RecordingDpHost;

class TransformEditorPreviewTestPeer {
 public:
  static void bind(
      TransformEditorToolbox& editor, RecordingDpHost& dp, FakeCatalogHost& catalog, FakePlaybackHost& playback,
      FakePlotTabsHost& tabs) {
    editor.dp_view_ = dp.view();
    editor.test_catalog_host_ = PJ::sdk::ToolboxHostView(catalog.makeHost());
    editor.playback_view_ = playback.view();
    editor.plot_tabs_view_ = tabs.view();
  }
  static TransformEditorDialog& dialog(TransformEditorToolbox& editor) {
    return editor.dialog_;
  }
  static void save(TransformEditorToolbox& editor) {
    editor.onSave();
  }
  static void refresh(TransformEditorToolbox& editor) {
    editor.catalog_refresh_ticks_ = 0;  // re-read the catalog now
    editor.refreshPreview();
  }
  static void showInScene(TransformEditorToolbox& editor) {
    editor.showInScene();
  }
  static int builds(const TransformEditorToolbox& editor) {
    return editor.on_demand_build_count_;
  }
  static const std::string& paramsJson(TransformEditorToolbox& editor) {
    return editor.buildOnDemandRequest().request.params_json;
  }
  static void tick(TransformEditorToolbox& editor) {
    editor.refreshPreview();
  }
  static nlohmann::json widgets(TransformEditorToolbox& editor) {
    return nlohmann::json::parse(editor.dialog_.widget_data());
  }
};

// An on-demand editor state the way the host stores it: the user's params object plus "__editor".
std::string onDemandConfig(
    const std::string& name, const nlohmann::json& outputs, const std::string& params_text = "{\"k\":2}",
    bool pin = false) {
  nlohmann::json editor = {
      {"output_name", name},
      {"global_code", ""},
      {"function_body", "return { cropped = inputs[\"/cloud\"], count = 1 }"},
      {"sources", nlohmann::json::array({"/cloud"})},
      {"primary_index", 0},
      {"language", "luau"},
      {"mode", "single"},
      {"kind", "on_demand"},
      {"outputs", outputs},
      {"params_text", params_text},
      {"pin_current_time", pin},
  };
  nlohmann::json params = nlohmann::json::parse(params_text);
  params["__editor"] = editor;
  return params.dump();
}

const nlohmann::json kCloudAndCount =
    nlohmann::json::array({{{"name", "cropped"}, {"type", "kPointCloud"}}, {{"name", "count"}, {"type", "number"}}});

struct Rig {
  RecordingDpHost dp;
  FakeCatalogHost catalog;
  FakePlaybackHost playback;
  FakePlotTabsHost tabs;
  TransformEditorToolbox editor;

  Rig() {
    catalog.addObjectTopic("/cloud", "kPointCloud", 5, 0, 5'000'000'000);
    playback.state.current_time_s = 3.0;
    TransformEditorPreviewTestPeer::bind(editor, dp, catalog, playback, tabs);
  }
  TransformEditorDialog& dialog() {
    return TransformEditorPreviewTestPeer::dialog(editor);
  }
  // The fakes pick their vtable when view() is called, so a test that changes a capability
  // flag rebinds afterwards.
  void rebind() {
    TransformEditorPreviewTestPeer::bind(editor, dp, catalog, playback, tabs);
  }
  void load(const std::string& config) {
    ASSERT_TRUE(dialog().loadConfig(config));
  }
};

}  // namespace

TEST(TransformEditorOnDemand, CreateSendsKindOutputsParamsEditorStateInstantAndNoFlags) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", kCloudAndCount, "{\"k\":2}", /*pin=*/true));
  TransformEditorPreviewTestPeer::save(rig.editor);

  ASSERT_EQ(rig.dp.create_v2_calls, 1);
  EXPECT_EQ(rig.dp.last_kind, "on_demand");
  EXPECT_EQ(rig.dp.last_id, "my_filter");
  EXPECT_EQ(rig.dp.last_create_v2_label, "my_filter");
  EXPECT_EQ(rig.dp.last_create_v2_flags, 0u) << "the user's own recipe is NOT history-exempt";
  EXPECT_EQ(rig.dp.last_create_v2_inputs, std::vector<std::string>{"/cloud"});
  EXPECT_EQ(rig.dp.last_outputs, (std::vector<std::string>{"cropped", "count"}));
  EXPECT_EQ(rig.dp.last_output_types, (std::vector<std::string>{"kPointCloud", "number"}));
  EXPECT_EQ(rig.dp.last_create_v2_time_flags, static_cast<std::uint32_t>(PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT));
  EXPECT_EQ(rig.dp.last_create_v2_instant_ns, 3'000'000'000);  // playhead 3 s through the shared toRawNs
  EXPECT_NE(rig.dp.last_create_v2_script.find("local inputs, params = ..."), std::string::npos);

  const auto params = nlohmann::json::parse(rig.dp.last_create_v2_params_json);
  EXPECT_EQ(params["k"], 2);
  ASSERT_TRUE(params.contains("__editor"));
  EXPECT_EQ(params["__editor"]["kind"], "on_demand");
  EXPECT_EQ(params["__editor"]["output_name"], "my_filter");
}

TEST(TransformEditorOnDemand, CreateWithoutPinCarriesNoInstant) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", kCloudAndCount));
  TransformEditorPreviewTestPeer::save(rig.editor);
  ASSERT_EQ(rig.dp.create_v2_calls, 1);
  EXPECT_EQ(rig.dp.last_create_v2_time_flags, 0u);
}

TEST(TransformEditorOnDemand, ModifyOfAnOwnRecipeUpsertsTheSameId) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", kCloudAndCount));
  // Loading a populated state is Modify: the name is locked and the button says so.
  EXPECT_EQ(
      TransformEditorPreviewTestPeer::widgets(rig.editor)["pushButtonCreate"]["button_text"],
      "Modify On-demand Recipe");
  TransformEditorPreviewTestPeer::save(rig.editor);
  TransformEditorPreviewTestPeer::save(rig.editor);
  ASSERT_EQ(rig.dp.create_v2_calls, 2);
  EXPECT_EQ(rig.dp.last_id, "my_filter");
  ASSERT_EQ(rig.dp.created.size(), 2u);
  EXPECT_EQ(rig.dp.created[0].id, rig.dp.created[1].id);
}

TEST(TransformEditorOnDemand, LoadConfigReadsTheEditorStateAndKeepsTheUsersParams) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", kCloudAndCount, "{\"k\":2}"));
  EXPECT_TRUE(rig.dialog().isOnDemand());
  EXPECT_EQ(rig.dialog().outputName(), "my_filter");
  EXPECT_EQ(rig.dialog().sources(), std::vector<std::string>{"/cloud"});
  EXPECT_EQ(rig.dialog().validOutputs().size(), 2u);
  EXPECT_EQ(rig.dialog().paramsText(), "{\"k\":2}");
  const auto saved = nlohmann::json::parse(rig.dialog().saveConfig());
  EXPECT_EQ(saved["kind"], "on_demand");
  EXPECT_EQ(saved["outputs"].size(), 2u);
}

TEST(TransformEditorOnDemand, LoadConfigStillReadsTheLegacyTransformState) {
  Rig rig;
  rig.load(
      R"({"output_name":"dbl","global_code":"","function_body":"return value*2","sources":["a/x","b/y"],)"
      R"("primary_index":1,"language":"luau","mode":"single"})");
  EXPECT_FALSE(rig.dialog().isOnDemand());
  EXPECT_EQ(rig.dialog().outputName(), "dbl");
  EXPECT_EQ(rig.dialog().sourceSeries(), "b/y");
  EXPECT_EQ(rig.dialog().extraSources(), std::vector<std::string>{"a/x"});
  // A transform's saved state keeps today's shape: none of the on-demand keys.
  const auto saved = nlohmann::json::parse(rig.dialog().saveConfig());
  EXPECT_FALSE(saved.contains("kind"));
  EXPECT_FALSE(saved.contains("outputs"));
  EXPECT_FALSE(saved.contains("__editor"));
  EXPECT_EQ(saved["function_body"], "return value*2");
}

TEST(TransformEditorOnDemand, LegacyHeaderStateLoadsIntoTheForm) {
  Rig rig;
  rig.load(
      R"({"output_name":"old","global_code":"-- pj-kind: on_demand\n-- pj-outputs: cropped:kPointCloud,count:number\n)"
      R"(-- pj-params: {\"k\":1}\nlocal scale = 2","function_body":"return {count = scale}","sources":["/cloud"]})");
  EXPECT_TRUE(rig.dialog().isOnDemand());
  const auto outputs = rig.dialog().validOutputs();
  ASSERT_EQ(outputs.size(), 2u);
  EXPECT_EQ(outputs[0].name, "cropped");
  EXPECT_EQ(outputs[0].type, "kPointCloud");
  EXPECT_EQ(outputs[1].type, "number");
  EXPECT_EQ(rig.dialog().paramsText(), "{\"k\":1}");
  EXPECT_EQ(rig.dialog().globalCode(), "local scale = 2");  // header lines are gone, the rest stays
}

TEST(TransformEditorOnDemand, HostWithoutTheTypedSurfaceDisablesTheKindAndNeverCreates) {
  Rig rig;
  rig.dp.supports_v2 = false;  // a floor-level host: no create_data_processor_v2 / submit_evaluation
  rig.rebind();
  rig.load(onDemandConfig("my_filter", kCloudAndCount));
  TransformEditorPreviewTestPeer::refresh(rig.editor);
  const auto widgets = TransformEditorPreviewTestPeer::widgets(rig.editor);
  EXPECT_EQ(widgets["kindOnDemandButton"]["enabled"], false);
  EXPECT_EQ(widgets["pushButtonCreate"]["enabled"], false);
  EXPECT_NE(rig.dialog().ui_content().find("requires a PlotJuggler host with SDK 0.36 or newer"), std::string::npos);
  TransformEditorPreviewTestPeer::save(rig.editor);
  EXPECT_EQ(rig.dp.create_v2_calls, 0);
  EXPECT_EQ(rig.dp.submit_calls, 0);
}

TEST(TransformEditorOnDemand, SupportedHostEnablesTheKindAndOffersObjectTopicsWithTheirType) {
  Rig rig;
  rig.catalog.addObjectTopic("/img", "kImage", 3, 0, 1'000'000'000);
  rig.catalog.addObjectTopic("__markers__/x", "kPlotMarkers", 1, 0, 1'000'000'000);
  rig.load(onDemandConfig("my_filter", kCloudAndCount));
  TransformEditorPreviewTestPeer::refresh(rig.editor);
  const auto widgets = TransformEditorPreviewTestPeer::widgets(rig.editor);
  EXPECT_EQ(widgets["kindOnDemandButton"]["enabled"], true);
  // The picker lists object topics (marker sets excluded) and the inputs table has a Type column.
  const auto items = widgets["objectTopicCombo"]["items"];
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0], "/cloud  [kPointCloud]");
  EXPECT_EQ(widgets["tableSources"]["rows"][0][3], "kPointCloud");
  EXPECT_EQ(widgets["tableOutputs"]["rows"].size(), 2u);
}

TEST(TransformEditorOnDemand, CreateIsEnabledOnlyWithInputOutputBodyAndAValidScript) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", kCloudAndCount));
  TransformEditorPreviewTestPeer::refresh(rig.editor);
  EXPECT_EQ(TransformEditorPreviewTestPeer::widgets(rig.editor)["pushButtonCreate"]["enabled"], true);

  rig.dp.fail_validate = true;  // the host rejects the script: validateScript is part of the gate
  TransformEditorPreviewTestPeer::refresh(rig.editor);
  EXPECT_EQ(TransformEditorPreviewTestPeer::widgets(rig.editor)["pushButtonCreate"]["enabled"], false);
  rig.dp.fail_validate = false;

  rig.load(onDemandConfig("my_filter", nlohmann::json::array()));  // no outputs declared: the form seeds one
  rig.dialog().onClicked("buttonAddOutput");                       // empty name: nothing is added
  TransformEditorPreviewTestPeer::refresh(rig.editor);
  EXPECT_EQ(rig.dialog().validOutputs().size(), 1u);

  rig.dialog().onItemDeleteRequested("tableOutputs", 0);  // delete the only output
  TransformEditorPreviewTestPeer::refresh(rig.editor);
  EXPECT_EQ(TransformEditorPreviewTestPeer::widgets(rig.editor)["pushButtonCreate"]["enabled"], false);
}

TEST(TransformEditorOnDemand, UnresolvedInputBlocksCreate) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", kCloudAndCount));
  rig.dialog().onItemsDropped("tableSources", {"/missing"});
  TransformEditorPreviewTestPeer::save(rig.editor);
  EXPECT_EQ(rig.dp.create_v2_calls, 0);
}

TEST(TransformEditorOnDemand, ShowInSceneOpensA3dTabForPointCloudsAnd2dForAnnotations) {
  for (const auto& [type, scene_kind] :
       {std::pair<std::string, std::string>{"kPointCloud", "3d"},
        std::pair<std::string, std::string>{"kImageAnnotations", "2d"}}) {
    Rig rig;
    rig.load(onDemandConfig("my_filter", nlohmann::json::array({{{"name", "out"}, {"type", type}}})));
    TransformEditorPreviewTestPeer::save(rig.editor);
    ASSERT_EQ(rig.dp.create_v2_calls, 1);
    const auto widgets = TransformEditorPreviewTestPeer::widgets(rig.editor);
    EXPECT_EQ(widgets["buttonShowScene"]["visible"], true);
    EXPECT_EQ(widgets["buttonShowScene"]["button_text"], scene_kind == "2d" ? "Show in 2D" : "Show in 3D");
    TransformEditorPreviewTestPeer::showInScene(rig.editor);
    ASSERT_EQ(rig.tabs.tabs.size(), 1u);
    EXPECT_EQ(rig.tabs.tabs[0].kind, scene_kind);
    ASSERT_EQ(rig.tabs.tabs[0].topics.size(), 1u);
    EXPECT_EQ(rig.tabs.tabs[0].topics[0].topic, "out");
  }
}

TEST(TransformEditorOnDemand, NoSceneButtonWithoutSceneTabsOrObjectOutputs) {
  {
    Rig rig;
    rig.tabs.null_tail = true;  // a host with no scene workspace
    rig.rebind();
    rig.load(onDemandConfig("my_filter", kCloudAndCount));
    TransformEditorPreviewTestPeer::save(rig.editor);
    EXPECT_EQ(TransformEditorPreviewTestPeer::widgets(rig.editor)["buttonShowScene"]["visible"], false);
  }
  {
    Rig rig;  // numbers only: nothing to show in a scene
    rig.load(onDemandConfig("my_filter", nlohmann::json::array({{{"name", "n"}, {"type", "number"}}})));
    TransformEditorPreviewTestPeer::save(rig.editor);
    EXPECT_EQ(TransformEditorPreviewTestPeer::widgets(rig.editor)["buttonShowScene"]["visible"], false);
  }
}

TEST(TransformEditorOnDemand, TransformCreateIsUnchanged) {
  Rig rig;
  rig.load(R"({"output_name":"dbl","global_code":"","function_body":"return value*2","sources":["a/x"]})");
  TransformEditorPreviewTestPeer::save(rig.editor);
  EXPECT_EQ(rig.dp.create_v2_calls, 0);
  EXPECT_EQ(rig.dp.last_kind, "transform");
  EXPECT_EQ(rig.dp.last_id, "dbl");
}

TEST(TransformEditorOnDemand, PickerQualifiesNamesWhenSeveralDatasetsAreLoaded) {
  Rig rig;
  rig.catalog.addObjectTopic("/a", "kPointCloud", 1, 0, 1'000'000'000, "{}", "runA");
  rig.catalog.addObjectTopic("/b", "kImage", 1, 0, 1'000'000'000, "{}", "runB");
  rig.load(onDemandConfig("my_filter", kCloudAndCount));
  TransformEditorPreviewTestPeer::refresh(rig.editor);
  const auto items = TransformEditorPreviewTestPeer::widgets(rig.editor)["objectTopicCombo"]["items"];
  EXPECT_NE(std::find(items.begin(), items.end(), "runA:/a  [kPointCloud]"), items.end());
  EXPECT_NE(std::find(items.begin(), items.end(), "runB:/b  [kImage]"), items.end());
  // The qualified name the picker adds reads back as the object topic's type in the inputs table.
  rig.dialog().onItemsDropped("tableSources", {"runA:/a"});
  EXPECT_EQ(TransformEditorPreviewTestPeer::widgets(rig.editor)["tableSources"]["rows"][1][3], "kPointCloud");
}

TEST(TransformEditorOnDemand, ShowInSceneAttachesWithTheInputsDataset) {
  Rig rig;
  rig.catalog.addObjectTopic("/a", "kPointCloud", 1, 0, 1'000'000'000, "{}", "runA");
  rig.catalog.addObjectTopic("/b", "kPointCloud", 1, 0, 1'000'000'000, "{}", "runB");
  auto config = nlohmann::json::parse(
      onDemandConfig("my_filter", nlohmann::json::array({{{"name", "out"}, {"type", "kPointCloud"}}})));
  config["__editor"]["sources"] = nlohmann::json::array({"runA:/a"});
  rig.load(config.dump());
  TransformEditorPreviewTestPeer::save(rig.editor);
  TransformEditorPreviewTestPeer::showInScene(rig.editor);
  ASSERT_EQ(rig.tabs.tabs.size(), 1u);
  ASSERT_EQ(rig.tabs.tabs[0].topics.size(), 1u);
  EXPECT_EQ(rig.tabs.tabs[0].topics[0].topic, "out");
  EXPECT_EQ(rig.tabs.tabs[0].topics[0].dataset, "runA");
}

TEST(TransformEditorOnDemand, ParamsMayNotUseTheReservedEditorKey) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", kCloudAndCount, "{\"__editor\":1}"));
  // The loader treats "__editor" in the stored document as the editor state, so set the field the way a user would.
  rig.dialog().onTextChanged("paramsLineEdit", "{\"__editor\":1}");
  TransformEditorPreviewTestPeer::refresh(rig.editor);
  const auto widgets = TransformEditorPreviewTestPeer::widgets(rig.editor);
  EXPECT_EQ(widgets["pushButtonCreate"]["enabled"], false);
  TransformEditorPreviewTestPeer::save(rig.editor);
  EXPECT_EQ(rig.dp.create_v2_calls, 0);
}

TEST(TransformEditorOnDemand, PreviewTicksReuseTheResolvedBuild) {
  Rig rig;
  rig.load(onDemandConfig("my_filter", kCloudAndCount));
  TransformEditorPreviewTestPeer::refresh(rig.editor);
  const int after_first = TransformEditorPreviewTestPeer::builds(rig.editor);
  ASSERT_GE(after_first, 1);
  for (int i = 0; i < 5; ++i) {
    TransformEditorPreviewTestPeer::tick(rig.editor);
  }
  EXPECT_EQ(TransformEditorPreviewTestPeer::builds(rig.editor), after_first) << "unchanged form: cached build";
  rig.dialog().onTextChanged("paramsLineEdit", "{\"k\":3}");
  EXPECT_NE(TransformEditorPreviewTestPeer::paramsJson(rig.editor).find("\"k\":3"), std::string::npos);
  EXPECT_EQ(TransformEditorPreviewTestPeer::builds(rig.editor), after_first + 1);
  TransformEditorPreviewTestPeer::refresh(rig.editor);  // a catalog re-read drops the cache
  EXPECT_EQ(TransformEditorPreviewTestPeer::builds(rig.editor), after_first + 2);
}
