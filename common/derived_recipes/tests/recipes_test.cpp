// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The pure helpers of the shared recipe library. Path resolution against a catalog is covered by
// the assistant's tool_registry tests (they drive it through the same functions); this file
// pins the text and parsing contracts both consumers rely on.
#include "derived_recipes/recipes.hpp"

#include <gtest/gtest.h>

using namespace derived_recipes;

TEST(DerivedRecipes, ParseTypedOutputsSplitsNameAndType) {
  const auto parsed = parseTypedOutputs(nlohmann::json::array({"cropped:kPointCloud", "count:number"}));
  ASSERT_TRUE(parsed.error.empty()) << parsed.error;
  ASSERT_EQ(parsed.outputs.size(), 2u);
  EXPECT_EQ(parsed.outputs[0].name, "cropped");
  EXPECT_EQ(parsed.outputs[0].type, "kPointCloud");
  EXPECT_EQ(parsed.outputs[1].type, "number");
}

TEST(DerivedRecipes, ParseTypedOutputsRejectsMalformedEntries) {
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array()).error.empty());
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array({"noType"})).error.empty());
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array({":number"})).error.empty());
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array({"name:"})).error.empty());
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array({1})).error.empty());
}

TEST(DerivedRecipes, LuaStringEscapeProtectsTheLiteral) {
  EXPECT_EQ(luaStringEscape("a\"b\\c\nd"), "a\\\"b\\\\c\\nd");
}

TEST(DerivedRecipes, OnDemandChunkBindsInputsAndParamsFromVarargs) {
  const std::string chunk = buildOnDemandChunk("return {}");
  EXPECT_EQ(chunk, "-- pj-script: luau\nlocal inputs, params = ...\nreturn {}\n");
}

TEST(DerivedRecipes, ResolvedChunkAliasesEveryScriptKeyToTheQualifiedRequestKey) {
  ResolvedEvalInputs resolved;
  resolved.aliases = {{"/cloud", "run1:/cloud"}, {"run1:/cloud", "run1:/cloud"}};
  const std::string chunk = buildResolvedOnDemandChunk("return {}", resolved);
  EXPECT_NE(chunk.find("[\"/cloud\"] = inputs[\"run1:/cloud\"]"), std::string::npos) << chunk;
  EXPECT_NE(chunk.find("local inputs, params = ...\ninputs = {"), std::string::npos) << chunk;
  EXPECT_NE(chunk.find("return {}"), std::string::npos);
}

TEST(DerivedRecipes, LuauTransformKeepsTheValueAndExtraInputParameters) {
  const std::string script = buildLuauTransform("id", "name", "g = 1", "return value + v1", 1);
  EXPECT_NE(script.find("return function(time, value, v1)"), std::string::npos) << script;
  EXPECT_NE(script.find("id = \"id\""), std::string::npos);
}

TEST(DerivedRecipes, ToRawNsIsEmptyWithoutAPlaybackView) {
  PJ::sdk::PlaybackHostView unbound;
  EXPECT_FALSE(toRawNs(unbound, PJ::sdk::DataSourceHandle{}, 1.0).has_value());
}

TEST(DerivedRecipes, QualifyAndJoinHelpers) {
  EXPECT_EQ(qualifyWithDataset("", "a/b"), "a/b");
  EXPECT_EQ(qualifyWithDataset("run1", "a/b"), "run1:a/b");
  EXPECT_EQ(joinSeriesPath("topic", "/field"), "topic/field");
  EXPECT_EQ(joinSeriesPath("topic", "field"), "topic/field");
  EXPECT_EQ(canonicalSeriesPath("a//b///c"), "a/b/c");
  EXPECT_TRUE(isMarkerObjectTopic("__markers__/x"));
  EXPECT_FALSE(isMarkerObjectTopic("/cloud"));
}

// --- Python on-demand chunk ---

TEST(DerivedRecipes, PythonChunkDefinesEvaluateWithIndentedBody) {
  ResolvedEvalInputs resolved;
  const std::string chunk = buildOnDemandChunkPython("x = 1\n\nreturn {'a': x}", resolved);
  EXPECT_EQ(chunk, "# pj-script: python\ndef evaluate(inputs, params):\n    x = 1\n\n    return {'a': x}\n");
}

TEST(DerivedRecipes, PythonChunkAliasesInputsAndKeepsAnEmptyBodyValid) {
  ResolvedEvalInputs resolved;
  resolved.aliases = {{"/cloud", "run1:/cloud"}};
  const std::string chunk = buildOnDemandChunkPython("", resolved);
  EXPECT_NE(chunk.find("def evaluate(inputs, params):\n    inputs = {\n"), std::string::npos) << chunk;
  EXPECT_NE(chunk.find("\"/cloud\": inputs[\"run1:/cloud\"],"), std::string::npos) << chunk;
  EXPECT_EQ(chunk.find("\n    pass"), std::string::npos) << "the alias table is already a statement";
  EXPECT_NE(buildOnDemandChunkPython("  \n", ResolvedEvalInputs{}).find("    pass\n"), std::string::npos);
}

// --- Variable names ---

TEST(DerivedRecipes, VariablePrologueBindsOneLocalPerInput) {
  const std::vector<InputBinding> bindings = {{"cloud", "/lidar_top"}, {"x", "pose/x"}};
  EXPECT_EQ(
      buildVariablePrologue("luau", bindings), "local cloud = inputs[\"/lidar_top\"]\nlocal x = inputs[\"pose/x\"]\n");
  EXPECT_EQ(buildVariablePrologue("python", bindings), "cloud = inputs[\"/lidar_top\"]\nx = inputs[\"pose/x\"]\n");
  EXPECT_EQ(buildVariablePrologue("luau", {}), "");
}

TEST(DerivedRecipes, InferredVariableNameIsTheSanitizedLeaf) {
  EXPECT_EQ(inferredVariableName("/lidar_top", {}), "lidar_top");
  EXPECT_EQ(inferredVariableName("pose/x", {}), "x");
  EXPECT_EQ(inferredVariableName("run1:/cam_front/image_rect", {}), "image_rect");
  EXPECT_EQ(inferredVariableName("run1:cloud", {}), "cloud");
  EXPECT_EQ(inferredVariableName("/a/b-c d", {}), "b_c_d");
  EXPECT_EQ(inferredVariableName("/cloud/", {}), "cloud");
  EXPECT_EQ(inferredVariableName("/3d", {}), "input_3d");
  EXPECT_EQ(inferredVariableName("/", {}), "input");
}

TEST(DerivedRecipes, InferredVariableNameDeduplicatesAndAvoidsKeywords) {
  EXPECT_EQ(inferredVariableName("/a/x", {"x"}), "x_2");
  EXPECT_EQ(inferredVariableName("/b/x", {"x", "x_2"}), "x_3");
  EXPECT_EQ(inferredVariableName("/end", {}), "end_");
  EXPECT_EQ(inferredVariableName("/class", {}), "class_");
  EXPECT_EQ(inferredVariableName("/None", {}), "None_");
  EXPECT_EQ(inferredVariableName("/inputs", {}), "inputs_");
  EXPECT_EQ(inferredVariableName("/pj", {}), "pj_");
  EXPECT_EQ(inferredVariableName("/end", {"end_"}), "end__2");
}

TEST(DerivedRecipes, ObjectTypeLabelsAreReadable) {
  EXPECT_EQ(objectTypeLabel("kPointCloud"), "point cloud");
  EXPECT_EQ(objectTypeLabel("kImage"), "image");
  EXPECT_EQ(objectTypeLabel("kImageAnnotations"), "image annotations");
  EXPECT_EQ(objectTypeLabel("kSceneEntities"), "scene");
  EXPECT_EQ(objectTypeLabel("kFrameTransforms"), "transforms");
  EXPECT_EQ(objectTypeLabel("kCameraInfo"), "camera info");
  EXPECT_EQ(objectTypeLabel("number"), "number");
  EXPECT_EQ(objectTypeLabel("kNotAType"), "kNotAType");
}
