// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The pure helpers of the shared recipe library: the text and script-layout contracts its
// consumers rely on.
#include "derived_recipes/recipes.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "test_support/fake_catalog_host.hpp"
#include "test_support/fake_playback_viewport_hosts.hpp"

using namespace derived_recipes;
using toolbox_testing::FakeCatalogHost;
using toolbox_testing::FakePlaybackHost;

TEST(DerivedRecipes, LuaStringEscapeProtectsTheLiteral) {
  EXPECT_EQ(luaStringEscape("a\"b\\c\nd\re\tf"), "a\\\"b\\\\c\\nd\\re\\tf");
}

TEST(DerivedRecipes, OnDemandChunkBindsInputsAndParamsFromVarargs) {
  const std::string chunk = buildOnDemandChunk("return {}");
  EXPECT_EQ(chunk, "-- pj-script: luau\nlocal inputs, params = ...\nreturn {}\n");
}

TEST(DerivedRecipes, ResolvedChunkAliasesEveryScriptKeyToTheQualifiedRequestKey) {
  ResolvedEvalInputs resolved;
  resolved.aliases = {{"/cloud", "run1:/cloud"}, {"run1:/cloud", "run1:/cloud"}};
  const std::string chunk = buildResolvedOnDemandChunk("return {}", resolved).script;
  EXPECT_NE(chunk.find("[\"/cloud\"] = inputs[\"run1:/cloud\"]"), std::string::npos) << chunk;
  EXPECT_NE(chunk.find("local inputs, params = ...\ninputs = {"), std::string::npos) << chunk;
  EXPECT_NE(chunk.find("return {}"), std::string::npos);
}

TEST(DerivedRecipes, LuauTransformKeepsTheValueAndExtraInputParameters) {
  const std::string script = buildTransformScript("id", "name", "g = 1", "return value + v1", 1, "luau").script;
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
  const std::string chunk = buildOnDemandChunkPython("x = 1\n\nreturn {'a': x}", resolved).script;
  EXPECT_EQ(chunk, "# pj-script: python\ndef evaluate(inputs, params):\n    x = 1\n\n    return {'a': x}\n");
}

TEST(DerivedRecipes, PythonChunkAliasesInputsAndKeepsAnEmptyBodyValid) {
  ResolvedEvalInputs resolved;
  resolved.aliases = {{"/cloud", "run1:/cloud"}};
  const std::string chunk = buildOnDemandChunkPython("", resolved).script;
  EXPECT_NE(chunk.find("def evaluate(inputs, params):\n    inputs = {\n"), std::string::npos) << chunk;
  EXPECT_NE(chunk.find("\"/cloud\": inputs[\"run1:/cloud\"],"), std::string::npos) << chunk;
  EXPECT_EQ(chunk.find("\n    pass"), std::string::npos) << "the alias table is already a statement";
  EXPECT_NE(buildOnDemandChunkPython("  \n", ResolvedEvalInputs{}).script.find("    pass\n"), std::string::npos);
}

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

}  // namespace

// --- Script layout ---

TEST(DerivedRecipes, TransformLayoutPointsAtTheBodyAndTheGlobals) {
  const BuiltScript luau = buildTransformScript("id", "name", "g = 1\nh = 2", "local y = value\nreturn y", 1, "luau");
  EXPECT_EQ(luau.script.rfind("-- pj-script: luau\n", 0), 0u);
  EXPECT_EQ(luau.layout.globals_first_line, lineOf(luau.script, "g = 1"));
  EXPECT_EQ(luau.layout.globals_lines, 2);
  EXPECT_EQ(luau.layout.body_first_line, lineOf(luau.script, "local y = value"));
  EXPECT_EQ(luau.layout.body_lines, 2);
  const BuiltScript no_globals = buildTransformScript("id", "name", "", "return value", 0, "luau");
  EXPECT_EQ(no_globals.layout.globals_lines, 0);
  EXPECT_EQ(no_globals.layout.body_first_line, lineOf(no_globals.script, "return value"));

  const BuiltScript python = buildTransformScript("id", "name", "g = 1", "y = value\nreturn y", 0, "python");
  EXPECT_EQ(python.layout.globals_first_line, lineOf(python.script, "g = 1"));
  EXPECT_EQ(python.layout.body_first_line, lineOf(python.script, "    y = value"));
  const BuiltScript python_plain = buildTransformScript("id", "name", "", "return value", 0, "python");
  EXPECT_EQ(python_plain.layout.body_first_line, lineOf(python_plain.script, "    return value"));
  const BuiltScript python_empty = buildTransformScript("id", "name", "", "", 0, "python");
  EXPECT_EQ(python_empty.layout.body_first_line, lineOf(python_empty.script, "    return value"));
}

TEST(DerivedRecipes, TransformScriptEscapesTheIdAndName) {
  const BuiltScript built = buildTransformScript("a\"b", "n\nm", "", "return value", 0, "luau");
  EXPECT_NE(built.script.find("id = \"a\\\"b\""), std::string::npos) << built.script;
  EXPECT_NE(built.script.find("name = \"n\\nm\""), std::string::npos) << built.script;
}

TEST(DerivedRecipes, OnDemandChunksReportWhereTheBodyStarts) {
  ResolvedEvalInputs resolved;
  resolved.aliases = {{"/cloud", "run1:/cloud"}, {"run1:/cloud", "run1:/cloud"}};
  const BuiltScript luau = buildResolvedOnDemandChunk("local a = 1\nreturn {a}", resolved);
  EXPECT_EQ(luau.layout.body_first_line, lineOf(luau.script, "local a = 1"));
  EXPECT_EQ(luau.layout.body_lines, 2);
  const BuiltScript python = buildOnDemandChunkPython("a = 1\nreturn {'a': a}", resolved);
  EXPECT_EQ(python.layout.body_first_line, lineOf(python.script, "    a = 1"));
  EXPECT_EQ(python.layout.body_lines, 2);
  const BuiltScript bare = buildResolvedOnDemandChunk("return {}", ResolvedEvalInputs{});
  EXPECT_EQ(bare.layout.body_first_line, lineOf(bare.script, "return {}"));
}

// --- Catalog path resolution ---

namespace {

constexpr std::int64_t kSec = 1'000'000'000;

// A dataset whose every topic carries one float64 field "value".
void addDataset(FakeCatalogHost& host, const std::string& dataset, const std::vector<std::string>& topics) {
  for (const auto& topic : topics) {
    host.addTopic(topic, dataset).addField(topic, "value");
  }
}

// Two runs of the same robot: every topic name appears in both datasets.
void addTwoRuns(FakeCatalogHost& host) {
  addDataset(host, "run_monday.mcap", {"/imu", "/speed"});
  addDataset(host, "run_friday.mcap", {"/imu", "/speed"});
}

}  // namespace

// An abbreviated path resolves when only one series can match, and hands back the full path: a slightly
// wrong path costs no correction round-trip, and the host receives the real name.
TEST(DerivedRecipesPaths, AbbreviationResolvesWhenOnlyOneSeriesMatches) {
  FakeCatalogHost host;
  host.addTopic("/imu").addField("/imu", "x").addField("/imu", "y");
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto abbreviated = resolveSeriesPath(*catalog, "x");
  ASSERT_TRUE(abbreviated.resolved.has_value());
  EXPECT_FALSE(abbreviated.ambiguous);
  EXPECT_EQ(abbreviated.resolved->host_path, "/imu/x");
  EXPECT_EQ(abbreviated.resolved->path, "/imu/x");
  EXPECT_EQ(abbreviated.resolved->topic, "/imu");
  EXPECT_EQ(abbreviated.resolved->dataset, "");

  const auto exact = resolveSeriesPath(*catalog, "/imu/x");
  ASSERT_TRUE(exact.resolved.has_value());
  EXPECT_EQ(exact.resolved->handle.id, abbreviated.resolved->handle.id);
}

// ...but never by guessing: two candidates come back as a listed choice, because attaching a transform to
// the wrong signal looks like it worked.
TEST(DerivedRecipesPaths, AmbiguousAbbreviationListsCandidatesInsteadOfGuessing) {
  FakeCatalogHost host;
  host.addTopic("/left").addField("/left", "speed").addTopic("/right").addField("/right", "speed");
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = resolveSeriesPath(*catalog, "speed");
  EXPECT_FALSE(lookup.resolved.has_value()) << "picking one of two same-named signals silently is worse than failing";
  EXPECT_TRUE(lookup.ambiguous);
  EXPECT_EQ(lookup.candidates, (std::vector<std::string>{"/left/speed", "/right/speed"}));
  const std::string error = seriesLookupError("speed", lookup);
  EXPECT_NE(error.find("ambiguous"), std::string::npos) << error;
  EXPECT_NE(error.find("'/left/speed'"), std::string::npos) << error;
  EXPECT_NE(error.find("'/right/speed'"), std::string::npos) << error;
}

// Whole-segment matching: half a segment must NOT resolve, or "/im" would silently attach to "/imu/x".
TEST(DerivedRecipesPaths, PartialSegmentAndUnknownPathsDoNotResolve) {
  FakeCatalogHost host;
  host.addTopic("/imu").addField("/imu", "x").addField("/imu", "y");
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  for (const std::string path : {"/im", "/imu/nope", "x/y"}) {
    const auto lookup = resolveSeriesPath(*catalog, path);
    EXPECT_FALSE(lookup.resolved.has_value()) << path;
    EXPECT_FALSE(lookup.ambiguous) << path;
    EXPECT_TRUE(lookup.candidates.empty()) << path;
    EXPECT_EQ(seriesLookupError(path, lookup).rfind("unknown series '" + path + "'", 0), 0u) << path;
  }
}

// The host's own display convention, "dataset:topic/field", addresses one run's series when both runs share
// every topic name.
TEST(DerivedRecipesPaths, QualifiedPathPicksItsDataset) {
  FakeCatalogHost host;
  addTwoRuns(host);
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto monday = resolveSeriesPath(*catalog, "run_monday.mcap:/speed/value");
  const auto friday = resolveSeriesPath(*catalog, "run_friday.mcap:/speed/value");
  ASSERT_TRUE(monday.resolved.has_value());
  ASSERT_TRUE(friday.resolved.has_value());
  EXPECT_NE(monday.resolved->handle.topic.id, friday.resolved->handle.topic.id)
      << "both qualifiers resolved to the same underlying series";
  EXPECT_EQ(monday.resolved->path, "run_monday.mcap:/speed/value");
  EXPECT_EQ(monday.resolved->dataset, "run_monday.mcap");
  EXPECT_EQ(friday.resolved->path, "run_friday.mcap:/speed/value");
  EXPECT_EQ(friday.resolved->dataset, "run_friday.mcap");
  // The host's create interface addresses inputs by their bare name: that form survives the qualifier.
  EXPECT_EQ(friday.resolved->host_path, "/speed/value");
}

// The bare path used to resolve silently to whichever file loaded first, for reads and installs alike.
TEST(DerivedRecipesPaths, BarePathExistingInSeveralDatasetsIsRefused) {
  FakeCatalogHost host;
  addTwoRuns(host);
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = resolveSeriesPath(*catalog, "/speed/value");
  EXPECT_FALSE(lookup.resolved.has_value()) << "resolved to '" << lookup.resolved->path << "' instead of refusing";
  EXPECT_TRUE(lookup.ambiguous);
  // Candidates arrive qualified, copy-pasteable back as-is: the refusal becomes a one-round-trip correction.
  EXPECT_EQ(
      lookup.candidates, (std::vector<std::string>{"run_monday.mcap:/speed/value", "run_friday.mcap:/speed/value"}));
  const std::string error = seriesLookupError("/speed/value", lookup);
  EXPECT_NE(error.find("'run_monday.mcap:/speed/value'"), std::string::npos) << error;
  EXPECT_NE(error.find("'run_friday.mcap:/speed/value'"), std::string::npos) << error;
}

// A topic living in only one dataset keeps resolving bare, and the resolved path discloses its dataset while
// host_path stays the bare form the host understands.
TEST(DerivedRecipesPaths, BarePathResolvesWhenUniqueAcrossDatasets) {
  FakeCatalogHost host;
  addDataset(host, "run_monday.mcap", {"/imu"});
  addDataset(host, "run_friday.mcap", {"/speed"});
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = resolveSeriesPath(*catalog, "/speed/value");
  ASSERT_TRUE(lookup.resolved.has_value());
  EXPECT_EQ(lookup.resolved->path, "run_friday.mcap:/speed/value");
  EXPECT_EQ(lookup.resolved->host_path, "/speed/value");
  EXPECT_EQ(lookup.resolved->dataset, "run_friday.mcap");
}

// Stream datasets carry names like "[stream] UDP Server". Matching the qualifier against the known names
// (instead of parsing at ':') makes those work with no escaping rules.
TEST(DerivedRecipesPaths, StreamStyleDatasetNamesNeedNoEscaping) {
  FakeCatalogHost host;
  addDataset(host, "run_monday.mcap", {"/udp/data"});
  addDataset(host, "[stream] UDP Server", {"/udp/data"});
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = resolveSeriesPath(*catalog, "[stream] UDP Server:/udp/data/value");
  ASSERT_TRUE(lookup.resolved.has_value());
  EXPECT_EQ(lookup.resolved->path, "[stream] UDP Server:/udp/data/value");
}

// One dataset loaded: paths stay bare in both directions, no qualifier to learn.
TEST(DerivedRecipesPaths, SingleDatasetKeepsBarePaths) {
  FakeCatalogHost host;
  addDataset(host, "only.mcap", {"/imu", "/speed"});
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  EXPECT_TRUE(datasetByTopicIndex(*catalog).empty());
  const auto lookup = resolveSeriesPath(*catalog, "/speed/value");
  ASSERT_TRUE(lookup.resolved.has_value());
  EXPECT_EQ(lookup.resolved->path, "/speed/value");
  EXPECT_EQ(lookup.resolved->dataset, "");
}

// The longest known dataset name wins, so a name that is a prefix of another does not capture its paths.
TEST(DerivedRecipesPaths, QualifierMatchPrefersTheLongestDatasetName) {
  FakeCatalogHost host;
  addDataset(host, "B", {"/speed"});
  addDataset(host, "B:run", {"/speed", "/imu"});
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto topic_dataset = datasetByTopicIndex(*catalog);
  EXPECT_EQ(topic_dataset.size(), 3u);
  EXPECT_EQ(topic_dataset.at(0), "B");
  EXPECT_EQ(topic_dataset.at(2), "B:run");

  const QualifierMatch longest = matchDatasetQualifier(*catalog, "B:run:/speed/value");
  EXPECT_EQ(longest.bare, "/speed/value");
  EXPECT_EQ(longest.topic_lo, 1u);
  EXPECT_EQ(longest.topic_hi, 3u);
  const QualifierMatch shorter = matchDatasetQualifier(*catalog, "B:/speed/value");
  EXPECT_EQ(shorter.bare, "/speed/value");
  EXPECT_EQ(shorter.topic_lo, 0u);
  EXPECT_EQ(shorter.topic_hi, 1u);
  const QualifierMatch none = matchDatasetQualifier(*catalog, "C:/speed/value");
  EXPECT_EQ(none.bare, "C:/speed/value");
  EXPECT_EQ(none.topic_lo, 0u);
  EXPECT_EQ(none.topic_hi, 3u);
}

// A resolved series' dataset name maps back to its source handle; an empty name is a source only when it is
// the sole one loaded.
TEST(DerivedRecipesPaths, DataSourceHandleForMatchesTheDatasetName) {
  FakeCatalogHost two;
  addTwoRuns(two);
  const PJ::sdk::ToolboxHostView two_view(two.makeHost());
  const auto two_catalog = two_view.catalogSnapshot();
  ASSERT_TRUE(two_catalog);
  const auto friday = dataSourceHandleFor(*two_catalog, "run_friday.mcap");
  ASSERT_TRUE(friday.has_value());
  EXPECT_EQ(friday->id, 2u);
  EXPECT_FALSE(dataSourceHandleFor(*two_catalog, "run_tuesday.mcap").has_value());
  EXPECT_FALSE(dataSourceHandleFor(*two_catalog, "").has_value()) << "an empty name is ambiguous with two sources";

  FakeCatalogHost one;
  addDataset(one, "only.mcap", {"/imu"});
  const PJ::sdk::ToolboxHostView one_view(one.makeHost());
  const auto one_catalog = one_view.catalogSnapshot();
  ASSERT_TRUE(one_catalog);
  const auto only = dataSourceHandleFor(*one_catalog, "");
  ASSERT_TRUE(only.has_value());
  EXPECT_EQ(only->id, 1u);
}

// --- Object topics ---

TEST(DerivedRecipesObjects, ListedObjectTopicsCarryTheCatalogRowAndSkipMarkers) {
  FakeCatalogHost host;
  host.addTopic("/imu").addField("/imu", "x");
  host.addObjectTopic("/cloud", "kPointCloud", 42, 0, 5 * kSec);
  host.addObjectTopic("__markers__/imu/x", "kPlotMarkers", 1, 0, kSec);
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto v2 = view.catalogSnapshotV2();
  ASSERT_TRUE(v2);

  const auto listed = listObjectTopics(*v2);
  ASSERT_EQ(listed.size(), 1u) << "the marker set's own topic is drawn, not read";
  EXPECT_EQ(listed[0].name, "/cloud");
  EXPECT_EQ(listed[0].type, "kPointCloud");
  EXPECT_EQ(listed[0].qualified, "/cloud") << "no qualifier without several datasets";
  EXPECT_EQ(listed[0].info.entry_count, 42u);
  EXPECT_EQ(listed[0].info.time_min_ns, 0);
  EXPECT_EQ(listed[0].info.time_max_ns, 5 * kSec);

  EXPECT_FALSE(resolveObjectTopic(*v2, "__markers__/imu/x").resolved.has_value());
}

// An object topic existing on two datasets is refused when named bare, with the qualified candidates, and
// "dataset:name" picks one.
TEST(DerivedRecipesObjects, QualifierPicksOneObjectTopicAndBareNameIsAmbiguous) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, kSec, R"({"builtin_object_type":"kPointCloud"})", "runA");
  host.addObjectTopic("/cloud", "kImage", 7, 0, kSec, R"({"builtin_object_type":"kImage"})", "runB");
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto v2 = view.catalogSnapshotV2();
  ASSERT_TRUE(v2);

  const auto ambiguous = resolveObjectTopic(*v2, "/cloud");
  EXPECT_FALSE(ambiguous.resolved.has_value());
  EXPECT_TRUE(ambiguous.ambiguous);
  EXPECT_EQ(ambiguous.candidates, (std::vector<std::string>{"runA:/cloud", "runB:/cloud"}));
  const std::string error = objectLookupError("/cloud", ambiguous);
  EXPECT_NE(error.find("ambiguous"), std::string::npos) << error;
  EXPECT_NE(error.find("'runA:/cloud'"), std::string::npos) << error;
  EXPECT_NE(error.find("'runB:/cloud'"), std::string::npos) << error;

  const auto b = resolveObjectTopic(*v2, "runB:/cloud");
  ASSERT_TRUE(b.resolved.has_value());
  EXPECT_EQ(b.resolved->object_type, "kImage");
  EXPECT_EQ(b.resolved->entry_count, 7u);
  EXPECT_EQ(b.resolved->display_path, "runB:/cloud");
  EXPECT_EQ(b.resolved->host_path, "/cloud");
  const auto a = resolveObjectTopic(*v2, "runA:/cloud");
  ASSERT_TRUE(a.resolved.has_value());
  EXPECT_EQ(a.resolved->object_type, "kPointCloud");
}

// --- On-demand input resolution ---

// The dataset qualifier survives into the request key, and the script can address the input by its bare
// name, its requested name or its qualified one. "B:run" contains a ':' and must beat the shorter "B".
TEST(DerivedRecipesInputs, ObjectInputKeepsItsQualifiedDatasetAndEveryScriptAlias) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "A");
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "B:run");
  PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto v2 = view.catalogSnapshotV2();
  ASSERT_TRUE(v2);

  const auto resolved = resolveEvalInputs(view, *v2, {"B:run:/cloud"});
  ASSERT_TRUE(resolved.error.empty()) << resolved.error;
  ASSERT_EQ(resolved.inputs.size(), 1u);
  EXPECT_TRUE(resolved.inputs[0].is_object);
  EXPECT_EQ(resolved.inputs[0].host_path, "/cloud");
  EXPECT_EQ(resolved.inputs[0].request_path, "B:run:/cloud");
  ASSERT_TRUE(resolved.anchor_source.has_value());
  EXPECT_EQ(resolved.anchor_source->id, 2u);
  EXPECT_EQ(
      resolved.aliases,
      (std::map<std::string, std::string>{{"/cloud", "B:run:/cloud"}, {"B:run:/cloud", "B:run:/cloud"}}));

  const std::string chunk = buildResolvedOnDemandChunk("return {}", resolved).script;
  EXPECT_NE(chunk.find("[\"/cloud\"] = inputs[\"B:run:/cloud\"]"), std::string::npos) << chunk;
  EXPECT_NE(chunk.find("[\"B:run:/cloud\"] = inputs[\"B:run:/cloud\"]"), std::string::npos) << chunk;
}

// A scalar input is resolved like a curve path: abbreviations expand, and with several datasets it is
// requested under its qualified name and anchors the time conversion on its dataset.
TEST(DerivedRecipesInputs, ScalarInputResolvesThroughTheCatalogPath) {
  FakeCatalogHost single;
  single.addTopic("/imu").addField("/imu", "x");
  PJ::sdk::ToolboxHostView single_view(single.makeHost());
  const auto single_v2 = single_view.catalogSnapshotV2();
  ASSERT_TRUE(single_v2);
  const auto bare = resolveEvalInputs(single_view, *single_v2, {"x"});
  ASSERT_TRUE(bare.error.empty()) << bare.error;
  ASSERT_EQ(bare.inputs.size(), 1u);
  EXPECT_FALSE(bare.inputs[0].is_object);
  EXPECT_EQ(bare.inputs[0].host_path, "/imu/x");
  EXPECT_EQ(bare.inputs[0].request_path, "/imu/x");
  EXPECT_FALSE(bare.anchor_source.has_value());
  EXPECT_EQ(bare.aliases.at("x"), "/imu/x");

  FakeCatalogHost multi;
  addDataset(multi, "run_monday.mcap", {"/imu"});
  addDataset(multi, "run_friday.mcap", {"/speed"});
  PJ::sdk::ToolboxHostView multi_view(multi.makeHost());
  const auto multi_v2 = multi_view.catalogSnapshotV2();
  ASSERT_TRUE(multi_v2);
  const auto qualified = resolveEvalInputs(multi_view, *multi_v2, {"/speed/value"});
  ASSERT_TRUE(qualified.error.empty()) << qualified.error;
  ASSERT_EQ(qualified.inputs.size(), 1u);
  EXPECT_EQ(qualified.inputs[0].host_path, "/speed/value");
  EXPECT_EQ(qualified.inputs[0].request_path, "run_friday.mcap:/speed/value");
  ASSERT_TRUE(qualified.anchor_source.has_value());
  EXPECT_EQ(qualified.anchor_source->id, 2u);
}

// An input naming nothing, or several things, is refused before anything can be installed.
TEST(DerivedRecipesInputs, UnknownAndAmbiguousInputsAreRefused) {
  FakeCatalogHost host;
  addTwoRuns(host);
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "run_monday.mcap");
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "run_friday.mcap");
  PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto v2 = view.catalogSnapshotV2();
  ASSERT_TRUE(v2);

  const auto unknown = resolveEvalInputs(view, *v2, {"/imu/does_not_exist"});
  EXPECT_NE(unknown.error.find("unknown series"), std::string::npos) << unknown.error;
  EXPECT_TRUE(unknown.inputs.empty());
  const auto scalar = resolveEvalInputs(view, *v2, {"/speed/value"});
  EXPECT_NE(scalar.error.find("ambiguous"), std::string::npos) << scalar.error;
  const auto object = resolveEvalInputs(view, *v2, {"/cloud"});
  EXPECT_NE(object.error.find("ambiguous"), std::string::npos) << object.error;
  EXPECT_NE(object.error.find("'run_friday.mcap:/cloud'"), std::string::npos) << object.error;
}

// One on-demand evaluation reads one dataset: inputs spread over two are rejected before the host is
// touched.
TEST(DerivedRecipesInputs, InputsFromDifferentDatasetsAreRejected) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "A");
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "B");
  host.addObjectTopic("/camera", "kImage", 2, 0, kSec, "{}", "B");
  PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto v2 = view.catalogSnapshotV2();
  ASSERT_TRUE(v2);

  const auto resolved = resolveEvalInputs(view, *v2, {"A:/cloud", "B:/camera"});
  EXPECT_NE(resolved.error.find("one dataset"), std::string::npos) << resolved.error;
}

// Two inputs whose script aliases collide on different requests would silently overwrite one another in the
// generated alias table: refused.
TEST(DerivedRecipesInputs, AliasesCannotOverwriteAnotherQualifiedInput) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "A");
  host.addObjectTopic("A:/cloud", "kPointCloud", 2, 0, kSec, "{}", "A");
  PJ::sdk::ToolboxHostView view(host.makeHost());
  const auto v2 = view.catalogSnapshotV2();
  ASSERT_TRUE(v2);

  const auto resolved = resolveEvalInputs(view, *v2, {"A:/cloud", "A:A:/cloud"});
  EXPECT_NE(resolved.error.find("ambiguous input alias"), std::string::npos) << resolved.error;
}

// --- Display <-> raw time ---

// Display seconds convert back to the dataset's raw ns through one forward conversion of raw 0.
TEST(DerivedRecipes, ToRawNsInvertsTheSourceScopedDisplayConversion) {
  FakePlaybackHost pb;
  pb.display_offset_ns = -2 * kSec;  // display = raw + 2 s
  PJ::sdk::PlaybackHostView playback = pb.view();
  PJ::sdk::DataSourceHandle source;
  source.id = 2;

  const auto raw = toRawNs(playback, source, 5.0);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(*raw, 3 * kSec);
  EXPECT_EQ(pb.last_source_id, 2u) << "converted against the input's own dataset";
}
