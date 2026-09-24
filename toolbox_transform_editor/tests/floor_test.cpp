// SPDX-License-Identifier: MPL-2.0
//
// Floor test backing manifest.json's suggested_sdk_version 0.35.0: proves the
// degraded path when the host predates submit_evaluation (ABI-APPENDED in
// 0.35.0 — struct_size stops just before it, the "0.34 size" a floor-level
// host would report). This is the exact host call previewOnDemand() makes in
// transform_editor_plugin.cpp to run an on-demand preview; the plugin's own
// classes are file-local (anonymous namespace, no public entry point to unit
// test), so this exercises the SDK view call one level below the UI, per the
// helper-level allowance in the porting policy for a floor test.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>

TEST(TransformEditorFloorTest, OnDemandSubmitReportsUnsupportedOnFloorHost) {
  PJ_data_processors_host_vtable_t vtable{};
  vtable.protocol_version = 1;
  vtable.struct_size = static_cast<uint32_t>(offsetof(PJ_data_processors_host_vtable_t, submit_evaluation));
  int ctx = 0;
  const PJ::sdk::DataProcessorsHostView host(PJ_data_processors_host_t{&ctx, &vtable});

  PJ::sdk::DataProcessorRequest request;
  request.id = "__te_preview";
  request.kind = "on_demand";
  request.language = "luau";
  request.script = "local inputs, params = ...\nreturn { result = 1 }";
  request.inputs = {"/lidar_top"};
  request.outputs = {{"result", "number"}};
  request.flags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL;
  request.instant_ns = 0;

  const auto handle = host.submitEvaluation(request, PJ::sdk::EvaluationBudget{.max_millis = 1000});
  ASSERT_FALSE(handle);  // no crash: a clean "unsupported" error instead
  EXPECT_NE(handle.error().find("not support"), std::string::npos) << handle.error();
}
