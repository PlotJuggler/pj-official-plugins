// SPDX-License-Identifier: MPL-2.0
//
// Floor test backing manifest.json's suggested_sdk_version 0.36.0: the editor itself (its source is
// compiled in, like async_preview_test) against a host that predates the typed data-processor requests
// and catalog snapshot v2, both ABI-APPENDED in 0.36.0. The series path keeps working, and an object
// input is refused with the reason, with nothing submitted or created.
#include <gtest/gtest.h>

#include "../transform_editor_plugin.cpp"
#include "test_support/fake_catalog_host.hpp"
#include "test_support/recording_dp_host.hpp"

namespace {

using toolbox_testing::FakeCatalogHost;
using toolbox_testing::RecordingDpHost;

class TransformEditorPreviewTestPeer {
 public:
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
  static void save(TransformEditorToolbox& editor) {
    editor.onSave();
  }
  static std::string createReason(TransformEditorToolbox& editor) {
    return editor.dialog_.canCreateReason();
  }
};

}  // namespace

TEST(TransformEditorFloorTest, TheSeriesPathStillCreatesATransformOnAFloorHost) {
  TransformEditorPreviewTestPeer::FloorHost host;
  TransformEditorToolbox editor;
  TransformEditorPreviewTestPeer::bind(editor, host);
  TransformEditorPreviewTestPeer::load(
      editor, R"({"output_name":"dbl","function_body":"return value*2","sources":["/imu/x"]})");
  EXPECT_EQ(TransformEditorPreviewTestPeer::createReason(editor), "");
  TransformEditorPreviewTestPeer::save(editor);
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
  TransformEditorPreviewTestPeer::save(editor);
  EXPECT_EQ(host.dp.persistent_creates, 0);
  EXPECT_EQ(host.dp.create_v2_calls, 0);
  EXPECT_EQ(host.dp.submit_calls, 0) << "no trial on a host without the typed requests";
}
