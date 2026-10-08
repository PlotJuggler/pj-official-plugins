// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The Transform Editor's script-text helpers: variable names, tokenizer, identifier renaming, the
// layout of the generated on-demand chunk and the remapping of host error lines.
#include "script_text.hpp"

#include <gtest/gtest.h>

#include <sstream>

using namespace derived_recipes;
using namespace transform_editor::text;

// --- Variable names ---

TEST(ScriptText, VariablePrologueBindsOneLocalPerInput) {
  const std::vector<InputBinding> bindings = {{"cloud", "/lidar_top"}, {"x", "pose/x"}};
  EXPECT_EQ(
      buildVariablePrologue("luau", bindings), "local cloud = inputs[\"/lidar_top\"]\nlocal x = inputs[\"pose/x\"]\n");
  EXPECT_EQ(buildVariablePrologue("python", bindings), "cloud = inputs[\"/lidar_top\"]\nx = inputs[\"pose/x\"]\n");
  EXPECT_EQ(buildVariablePrologue("luau", {}), "");
}

TEST(ScriptText, InferredVariableNameIsTheSanitizedLeaf) {
  EXPECT_EQ(inferredVariableName("/lidar_top", {}), "lidar_top");
  EXPECT_EQ(inferredVariableName("pose/x", {}), "x");
  EXPECT_EQ(inferredVariableName("run1:/cam_front/image_rect", {}), "image_rect");
  EXPECT_EQ(inferredVariableName("run1:cloud", {}), "cloud");
  EXPECT_EQ(inferredVariableName("/a/b-c d", {}), "b_c_d");
  EXPECT_EQ(inferredVariableName("/cloud/", {}), "cloud");
  EXPECT_EQ(inferredVariableName("/3d", {}), "input_3d");
  EXPECT_EQ(inferredVariableName("/", {}), "input");
}

TEST(ScriptText, InferredVariableNameDeduplicatesAndAvoidsKeywords) {
  EXPECT_EQ(inferredVariableName("/a/x", {"x"}), "x_2");
  EXPECT_EQ(inferredVariableName("/b/x", {"x", "x_2"}), "x_3");
  EXPECT_EQ(inferredVariableName("/end", {}), "end_");
  EXPECT_EQ(inferredVariableName("/class", {}), "class_");
  EXPECT_EQ(inferredVariableName("/None", {}), "None_");
  EXPECT_EQ(inferredVariableName("/inputs", {}), "inputs_");
  EXPECT_EQ(inferredVariableName("/pj", {}), "pj_");
  EXPECT_EQ(inferredVariableName("/end", {"end_"}), "end__2");
}

TEST(ScriptText, ObjectTypeLabelsAreReadable) {
  EXPECT_EQ(objectTypeLabel("kPointCloud"), "point cloud");
  EXPECT_EQ(objectTypeLabel("kImage"), "image");
  EXPECT_EQ(objectTypeLabel("kImageAnnotations"), "image annotations");
  EXPECT_EQ(objectTypeLabel("kSceneEntities"), "scene");
  EXPECT_EQ(objectTypeLabel("kFrameTransforms"), "transforms");
  EXPECT_EQ(objectTypeLabel("kCameraInfo"), "camera info");
  EXPECT_EQ(objectTypeLabel("number"), "number");
  EXPECT_EQ(objectTypeLabel("kNotAType"), "kNotAType");
}

// --- Code tokens ---

namespace {

// The 1-based number of the first line of `script` that is exactly `line`.
int lineOf(const std::string& script, const std::string& line) {
  std::istringstream in(script);
  std::string text;
  for (int n = 1; std::getline(in, text); ++n) {
    if (text == line) {
      return n;
    }
  }
  return -1;
}

std::string kinds(const std::string& text, const char* language) {
  std::string out;
  for (const CodeToken& token : tokenize(text, language)) {
    switch (token.kind) {
      case CodeTokenKind::kWord:
        out += 'w';
        break;
      case CodeTokenKind::kString:
        out += 's';
        break;
      case CodeTokenKind::kComment:
        out += 'c';
        break;
      case CodeTokenKind::kOther:
        out += '.';
        break;
    }
  }
  return out;
}

}  // namespace

TEST(ScriptText, TokensCoverStringsAndCommentsAsOneToken) {
  EXPECT_EQ(kinds("a=\"x y\"", "luau"), "w.s");
  EXPECT_EQ(kinds("a --[[ x\ny ]] b", "luau"), "w.c.w");
  EXPECT_EQ(kinds("-- one\nb", "luau"), "c.w");
  EXPECT_EQ(kinds("s=[==[a]]b]==]", "luau"), "w.s");
  EXPECT_EQ(kinds("t[1]", "luau"), "w.w.");
  EXPECT_EQ(kinds("s='''a\nb'''", "python"), "w.s");
  EXPECT_EQ(kinds("a # c\nb", "python"), "w.c.w");
  EXPECT_EQ(kinds("a -- b", "python"), "w....w") << "-- is not a comment in Python";
}

TEST(ScriptText, ReturnArityReadsTheReturnStatements) {
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

TEST(ScriptText, ReturnArityIgnoresLuauLongStringsAndPythonTripleQuotes) {
  EXPECT_EQ(returnArity("local s = [[return a, b, c]]\nreturn a", "luau"), 1u);
  EXPECT_EQ(returnArity("--[[ return a, b, c ]]\nreturn a, b", "luau"), 2u);
  EXPECT_EQ(returnArity("--[==[\nreturn 1, 2, 3\n]==] return a", "luau"), 1u);
  EXPECT_EQ(returnArity("return [[a, b]], c", "luau"), 2u);
  EXPECT_EQ(returnArity("s = '''\nreturn a, b, c\n'''\nreturn a", "python"), 1u);
  EXPECT_EQ(returnArity("return \"\"\"x, y\"\"\", z", "python"), 2u);
}

// --- Identifiers ---

TEST(ScriptText, MentionsIdentifierSkipsStringsCommentsFieldsKeysAndKeywordArguments) {
  EXPECT_TRUE(mentionsIdentifier("return value + 1", "luau", "value"));
  EXPECT_FALSE(mentionsIdentifier("return values + 1", "luau", "value"));
  EXPECT_FALSE(mentionsIdentifier("return a.value + a:value()", "luau", "value"));
  EXPECT_FALSE(mentionsIdentifier("local s = \"value\" -- value", "luau", "value"));
  EXPECT_FALSE(mentionsIdentifier("local s = [[value]]\n--[[ value ]]", "luau", "value"));
  EXPECT_FALSE(mentionsIdentifier("return { value = 1, other = 2 }", "luau", "value"));
  EXPECT_FALSE(mentionsIdentifier("return { other = 2, value = 1 }", "luau", "value"));
  EXPECT_TRUE(mentionsIdentifier("return { x = value }", "luau", "value"));
  EXPECT_TRUE(mentionsIdentifier("local a, value = f()", "luau", "value"))
      << "a name in a local list is a variable, not a table key";
  EXPECT_TRUE(mentionsIdentifier("if value == 1 then end", "luau", "value"));
  EXPECT_TRUE(mentionsIdentifier("t = { [1] = 2 }\nreturn value", "luau", "value"));
  EXPECT_FALSE(mentionsIdentifier("f(value=1)", "python", "value"));
  EXPECT_TRUE(mentionsIdentifier("f(value)", "python", "value"));
  EXPECT_TRUE(mentionsIdentifier("f(x=value)", "python", "value"));
  EXPECT_TRUE(mentionsIdentifier("value = 1", "python", "value"));
  EXPECT_FALSE(mentionsIdentifier("x = '''value'''  # value", "python", "value"));
  EXPECT_FALSE(mentionsIdentifier("x = a.value", "python", "value"));
}

TEST(ScriptText, RenameIdentifiersTouchesOnlyIdentifierUses) {
  const std::map<std::string, std::string> renames = {{"cloud", "lidar"}};
  EXPECT_EQ(renameIdentifiers("return cloud.count", "luau", renames), "return lidar.count");
  EXPECT_EQ(
      renameIdentifiers("local n = cloud:count() + cloud_2.x + cloud", "luau", renames),
      "local n = lidar:count() + cloud_2.x + lidar");
  EXPECT_EQ(
      renameIdentifiers("-- cloud\nlocal s = \"cloud\" .. [[cloud]]\nreturn cloud", "luau", renames),
      "-- cloud\nlocal s = \"cloud\" .. [[cloud]]\nreturn lidar");
  EXPECT_EQ(renameIdentifiers("return { cloud = cloud, n = 1 }", "luau", renames), "return { cloud = lidar, n = 1 }");
  EXPECT_EQ(renameIdentifiers("return cloud.cloud", "luau", renames), "return lidar.cloud");
  EXPECT_EQ(renameIdentifiers("f(cloud=cloud)", "python", renames), "f(cloud=lidar)");
  EXPECT_EQ(
      renameIdentifiers("x = '''cloud\n'''\nreturn cloud  # cloud", "python", renames),
      "x = '''cloud\n'''\nreturn lidar  # cloud");
  EXPECT_EQ(renameIdentifiers("return cloud", "luau", {}), "return cloud");
}

TEST(ScriptText, RenameIdentifiersSeesANameAfterAConcatenation) {
  const std::map<std::string, std::string> renames = {{"cloud", "lidar"}};
  EXPECT_EQ(renameIdentifiers("return \"n=\" .. cloud", "luau", renames), "return \"n=\" .. lidar");
  EXPECT_EQ(renameIdentifiers("return a..cloud..b", "luau", renames), "return a..lidar..b");
  EXPECT_EQ(renameIdentifiers("return x.cloud", "luau", renames), "return x.cloud");
  EXPECT_EQ(renameIdentifiers("return \"n=\" .. x.cloud", "luau", renames), "return \"n=\" .. x.cloud");
  EXPECT_TRUE(mentionsIdentifier("return \"n=\" .. cloud", "luau", "cloud"));
  EXPECT_FALSE(mentionsIdentifier("return x.cloud", "luau", "cloud"));
}

TEST(ScriptText, RenameIdentifiersAppliesAllRenamesAtOnce) {
  const std::map<std::string, std::string> swap = {{"a", "b"}, {"b", "a"}};
  EXPECT_EQ(renameIdentifiers("return a - b", "luau", swap), "return b - a");
}

TEST(ScriptText, VarNameErrorNamesTheReason) {
  EXPECT_EQ(varNameError("cloud", "luau", {"x"}), "");
  EXPECT_EQ(varNameError("_a1", "python", {}), "");
  EXPECT_NE(varNameError("", "luau", {}), "");
  EXPECT_NE(varNameError("1a", "luau", {}), "");
  EXPECT_NE(varNameError("a-b", "luau", {}), "");
  EXPECT_NE(varNameError("end", "luau", {}).find("reserved"), std::string::npos);
  EXPECT_NE(varNameError("inputs", "luau", {}).find("reserved"), std::string::npos);
  EXPECT_NE(varNameError("class", "python", {}).find("reserved"), std::string::npos);
  EXPECT_EQ(varNameError("class", "luau", {}), "") << "reserved by the other language only";
  EXPECT_NE(varNameError("x", "luau", {"x"}).find("already"), std::string::npos);
  EXPECT_TRUE(isReservedScriptName("end"));
  EXPECT_TRUE(isReservedScriptName("class"));
  EXPECT_FALSE(isReservedScriptName("cloud"));
}

// --- Script layout and error lines ---

TEST(ScriptText, LuauOnDemandLayoutPointsAtTheBodyAndTheGlobals) {
  ResolvedEvalInputs resolved;
  resolved.aliases = {{"/lidar_top", "/lidar_top"}};
  const std::vector<InputBinding> bindings = {{"lidar_top", "/lidar_top"}};
  const BuiltScript built = buildOnDemandScript("local n = 1\nreturn n", "G1 = 5\nG2 = 6", bindings, resolved, "luau");
  EXPECT_EQ(built.script.rfind("-- pj-script: luau\n", 0), 0u) << "the directive stays on line 1";
  EXPECT_EQ(built.layout.globals_first_line, lineOf(built.script, "G1 = 5"));
  EXPECT_EQ(built.layout.globals_lines, 2);
  EXPECT_EQ(built.layout.body_first_line, lineOf(built.script, "local n = 1"));
  EXPECT_EQ(built.layout.body_lines, 2);
  EXPECT_EQ(
      built.script, buildResolvedOnDemandChunk(
                        buildVariablePrologue("luau", bindings) + "G1 = 5\nG2 = 6\nlocal n = 1\nreturn n", resolved)
                        .script);

  const BuiltScript plain = buildOnDemandScript("return 1", "", {}, ResolvedEvalInputs{}, "luau");
  EXPECT_EQ(plain.layout.globals_lines, 0);
  EXPECT_EQ(plain.layout.body_first_line, lineOf(plain.script, "return 1"));
}

TEST(ScriptText, PythonOnDemandLayoutPointsAtTheBodyAndTheGlobals) {
  ResolvedEvalInputs resolved;
  resolved.aliases = {{"/lidar_top", "/lidar_top"}};
  const std::vector<InputBinding> bindings = {{"lidar_top", "/lidar_top"}};
  const BuiltScript built = buildOnDemandScript("x = 1\nreturn {'a': x}", "G = 5", bindings, resolved, "python");
  EXPECT_EQ(built.script.rfind("# pj-script: python\ndef evaluate(inputs, params):\n", 0), 0u);
  EXPECT_EQ(built.layout.globals_first_line, lineOf(built.script, "    G = 5"));
  EXPECT_EQ(built.layout.body_first_line, lineOf(built.script, "    x = 1"));
  EXPECT_EQ(built.layout.body_lines, 2);
  EXPECT_EQ(
      built.script,
      buildOnDemandChunkPython(buildVariablePrologue("python", bindings) + "G = 5\nx = 1\nreturn {'a': x}", resolved)
          .script);

  const BuiltScript bare = buildOnDemandScript("return 1", "", {}, ResolvedEvalInputs{}, "python");
  EXPECT_EQ(bare.layout.body_first_line, lineOf(bare.script, "    return 1"));
}

TEST(ScriptText, RemapScriptLinesRewritesLuauAndPythonLines) {
  ScriptLayout layout;
  layout.body_first_line = 8;
  layout.body_lines = 3;
  layout.globals_first_line = 6;
  layout.globals_lines = 2;
  EXPECT_EQ(remapScriptLines("script:8: attempt to index nil", layout), "line 1: attempt to index nil");
  EXPECT_EQ(remapScriptLines("script:10: boom", layout), "line 3: boom");
  EXPECT_EQ(remapScriptLines("script:7: boom", layout), "globals line 2: boom");
  EXPECT_EQ(remapScriptLines("script:2: boom", layout), "line ?: boom");
  EXPECT_EQ(remapScriptLines("script:11: boom", layout), "line ?: boom");
  EXPECT_EQ(remapScriptLines("filter:8: boom", layout), "line 1: boom");
  EXPECT_EQ(remapScriptLines("rule:9: boom", layout), "line 2: boom");
  EXPECT_EQ(
      remapScriptLines("invalid script: script:8: boom (script:9: inner)", layout),
      "invalid script: line 1: boom (line 2: inner)");
  EXPECT_EQ(remapScriptLines("Python error: boom <string>(9)", layout), "Python error: boom line 2");
  EXPECT_EQ(remapScriptLines("File \"<string>\", line 8, in evaluate", layout), "File line 1, in evaluate");
  EXPECT_EQ(remapScriptLines("'{' was never closed (<string>, line 8)", layout), "'{' was never closed (line 1)");
  EXPECT_EQ(remapScriptLines("invalid syntax (<string>, line 7)", layout), "invalid syntax (globals line 2)");
  EXPECT_EQ(remapScriptLines("invalid syntax (<string>, line 2)", layout), "invalid syntax (line ?)");
  EXPECT_EQ(remapScriptLines("see (<string>, line x)", layout), "see (<string>, line x)");
  EXPECT_EQ(
      remapScriptLines("t=12:30: no script here, javascript:8: nor", layout),
      "t=12:30: no script here, javascript:8: nor");
  EXPECT_EQ(remapScriptLines("no line numbers", layout), "no line numbers");
}

TEST(ScriptText, RemapScriptLinesFollowsTheGeneratedChunks) {
  // The host reports the chunk's own line number; the remap says "line 1" for the first body line of each layout.
  ResolvedEvalInputs resolved;
  resolved.aliases = {{"/c", "/c"}};
  const std::vector<InputBinding> bindings = {{"c", "/c"}};
  const BuiltScript luau = buildOnDemandScript("return c.count", "", bindings, resolved, "luau");
  EXPECT_EQ(
      remapScriptLines("script:" + std::to_string(lineOf(luau.script, "return c.count")) + ": x", luau.layout),
      "line 1: x");
  const BuiltScript python = buildOnDemandScript("return c.count", "", bindings, resolved, "python");
  EXPECT_EQ(
      remapScriptLines(
          "File \"<string>\", line " + std::to_string(lineOf(python.script, "    return c.count")), python.layout),
      "File line 1");
  const BuiltScript transform = buildTransformScript("i", "n", "", "return value", 0, "luau");
  EXPECT_EQ(
      remapScriptLines("script:" + std::to_string(lineOf(transform.script, "return value")) + ": x", transform.layout),
      "line 1: x");
}

TEST(ScriptText, NamesUndefinedNameMatchesThePinnedPhrases) {
  EXPECT_TRUE(namesUndefinedName("luau", "script:8: attempt to index nil with 'count'"));
  EXPECT_TRUE(namesUndefinedName("luau", "attempt to index a nil value"));
  EXPECT_TRUE(namesUndefinedName("luau", "attempt to call a nil value"));
  EXPECT_FALSE(namesUndefinedName("luau", "attempt to compare number < string"));
  EXPECT_FALSE(namesUndefinedName("luau", "NameError: name 'x' is not defined"));
  EXPECT_TRUE(namesUndefinedName("python", "NameError: name 'cloud' is not defined"));
  EXPECT_TRUE(namesUndefinedName("python", "AttributeError: 'NoneType' object has no attribute 'count'"));
  EXPECT_FALSE(namesUndefinedName("python", "ZeroDivisionError: division by zero"));
  EXPECT_FALSE(namesUndefinedName("python", "attempt to index nil"));
}
