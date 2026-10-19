#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <pj_can_dbc/dictionary_paths.hpp>
#include <string>
#include <vector>

#include "dictionary_test_files.hpp"

namespace {

using pj_can_dbc::auxBaseDir;
using pj_can_dbc::AuxPathContext;
using pj_can_dbc::readAuxFile;
using pj_can_dbc::resolveAuxPath;
using pj_can_dbc::resolveAuxPaths;
using pj_can_dbc::toConfigPath;
using pj_can_dbc::toConfigPaths;
using pj_can_dbc::testing::generic;
using pj_can_dbc::testing::ScratchDir;

#ifdef _WIN32
const std::string kRoot = "C:/";
#else
const std::string kRoot = "/";
#endif

namespace fs = std::filesystem;

void writeFile(const fs::path& file) {
  fs::create_directories(file.parent_path());
  std::ofstream(file) << "VERSION \"\"\n";
}

std::string at(const std::string& tail) {
  return kRoot + tail;
}

// ---------------------------------------------------------------------------
// toConfigPath
// ---------------------------------------------------------------------------

TEST(DictionaryPaths, ToConfigPathBesideDataIsBareName) {
  EXPECT_EQ(toConfigPath(at("run/vehicle.dbc"), at("run/data.blf")), "vehicle.dbc");
}

TEST(DictionaryPaths, ToConfigPathInSubfolderKeepsTheSubfolder) {
  EXPECT_EQ(toConfigPath(at("run/dbc/v.dbc"), at("run/data.blf")), "dbc/v.dbc");
}

TEST(DictionaryPaths, ToConfigPathOutsideTheDataFolderIsUnchanged) {
  EXPECT_EQ(toConfigPath(at("shared/v.dbc"), at("run/data.blf")), at("shared/v.dbc"));
}

TEST(DictionaryPaths, ToConfigPathSiblingWithSamePrefixIsNotUnder) {
  EXPECT_EQ(toConfigPath(at("run2/v.dbc"), at("run/data.blf")), at("run2/v.dbc"));
}

TEST(DictionaryPaths, ToConfigPathDotDotEscapeIsUnchanged) {
  EXPECT_EQ(toConfigPath(at("run/../other/v.dbc"), at("run/data.blf")), at("run/../other/v.dbc"));
}

TEST(DictionaryPaths, ToConfigPathNormalizesInnerDotDot) {
  EXPECT_EQ(toConfigPath(at("run/sub/../v.dbc"), at("run/data.blf")), "v.dbc");
}

TEST(DictionaryPaths, ToConfigPathLeavesUnanchoredInputsAlone) {
  EXPECT_EQ(toConfigPath("v.dbc", at("run/data.blf")), "v.dbc");
  EXPECT_EQ(toConfigPath(at("run/v.dbc"), ""), at("run/v.dbc"));
  EXPECT_EQ(toConfigPath(at("run/v.dbc"), "mcap://host/run/data.mcap"), at("run/v.dbc"));
  EXPECT_EQ(toConfigPath("https://example.com/run/v.dbc", at("run/data.blf")), "https://example.com/run/v.dbc");
}

#ifdef _WIN32
TEST(DictionaryPaths, ToConfigPathAcceptsBackslashes) {
  EXPECT_EQ(toConfigPath("C:\\run\\dbc\\v.dbc", "C:\\run\\data.blf"), "dbc/v.dbc");
}
#endif

// ---------------------------------------------------------------------------
// auxBaseDir
// ---------------------------------------------------------------------------

TEST(DictionaryPaths, AuxBaseDirIsTheAbsoluteDataDirectory) {
  EXPECT_EQ(auxBaseDir(at("run/sub/../data.blf")), at("run"));
}

TEST(DictionaryPaths, AuxBaseDirIsEmptyWithoutAnAbsoluteFilesystemDirectory) {
  EXPECT_EQ(auxBaseDir(""), "");
  EXPECT_EQ(auxBaseDir("data.blf"), "");
  EXPECT_EQ(auxBaseDir("s3://b/d.blf"), "");
}

// ---------------------------------------------------------------------------
// resolveAuxPath
// ---------------------------------------------------------------------------

TEST(DictionaryPaths, ResolveRelativeAgainstTheCurrentDataDirectory) {
  const AuxPathContext context{at("new/data.blf"), "", ""};
  EXPECT_EQ(resolveAuxPath("v.dbc", context), at("new/v.dbc"));
  EXPECT_EQ(resolveAuxPath("dbc/v.dbc", context), at("new/dbc/v.dbc"));
}

TEST(DictionaryPaths, ResolveRelativeMayLeaveTheDataDirectory) {
  const AuxPathContext context{at("new/data.blf"), "", ""};
  EXPECT_EQ(resolveAuxPath("../shared/v.dbc", context), at("shared/v.dbc"));
}

TEST(DictionaryPaths, ResolvePrefillWithBaseAndNoSavedKeyUsesTheSavedBase) {
  const AuxPathContext context{at("other/data.blf"), "", at("old")};
  EXPECT_EQ(resolveAuxPath("v.dbc", context), at("old/v.dbc"));
}

TEST(DictionaryPaths, ResolveReplayIgnoresTheBaseWhenTheHostReportedTheReplacedPath) {
  const AuxPathContext context{at("new/data.blf"), at("old/data.blf"), at("stale")};
  EXPECT_EQ(resolveAuxPath("v.dbc", context), at("new/v.dbc"));
}

TEST(DictionaryPaths, ResolveMovedAbsolutePathIsRebased) {
  const AuxPathContext context{at("new/data.blf"), at("old/data.blf"), ""};
  EXPECT_EQ(resolveAuxPath(at("old/v.dbc"), context), at("new/v.dbc"));
  EXPECT_EQ(resolveAuxPath(at("old/dbc/v.dbc"), context), at("new/dbc/v.dbc"));
}

TEST(DictionaryPaths, ResolveCopiedAbsolutePathIsRebasedEvenWhileTheOldFileExists) {
  ScratchDir scratch;
  const auto old_dir = scratch.root() / "old";
  const auto copy_dir = scratch.root() / "copy";
  writeFile(old_dir / "v.dbc");
  writeFile(copy_dir / "v.dbc");

  const AuxPathContext context{generic(copy_dir / "data.blf"), generic(old_dir / "data.blf"), ""};
  EXPECT_EQ(resolveAuxPath(generic(old_dir / "v.dbc"), context), generic(copy_dir / "v.dbc"));
}

TEST(DictionaryPaths, ResolveRebasedWinsWhenBothExist) {
  // A copied layout: the stored file and the copy's file both exist, and the copy's is used.
  ScratchDir scratch;
  const auto root = scratch.root();
  writeFile(root / "old/dbc/v.dbc");
  writeFile(root / "new/dbc/v.dbc");
  const AuxPathContext context{generic(root / "new/data/run.blf"), generic(root / "old/data/run.blf"), ""};
  EXPECT_EQ(resolveAuxPath(generic(root / "old/dbc/v.dbc"), context), generic(root / "new/dbc/v.dbc"));
}

TEST(DictionaryPaths, ResolveDeepCommonSuffixKeepsAStoredDictionaryThatDidNotMove) {
  // /home/u/logs/run.blf copied to /mnt/u/logs/run.blf: the common tail u/logs/run.blf makes the guess
  // /home -> /mnt, but the shared library /home/u/dbc/vehicle.dbc never moved and its rebased twin is absent.
  ScratchDir scratch;
  const auto root = scratch.root();
  writeFile(root / "home/u/dbc/vehicle.dbc");
  const AuxPathContext context{generic(root / "mnt/u/logs/run.blf"), generic(root / "home/u/logs/run.blf"), ""};
  const std::string stored = generic(root / "home/u/dbc/vehicle.dbc");
  EXPECT_EQ(resolveAuxPath(stored, context), stored);
}

TEST(DictionaryPaths, ResolveNeitherExistsNamesTheRebasedPath) {
  ScratchDir scratch;
  const auto root = scratch.root();
  const AuxPathContext context{generic(root / "new/data/run.blf"), generic(root / "old/data/run.blf"), ""};
  EXPECT_EQ(resolveAuxPath(generic(root / "old/dbc/v.dbc"), context), generic(root / "new/dbc/v.dbc"));
}

TEST(DictionaryPaths, ResolveClampKeepsTheFirstComponentBelowTheRoot) {
  // The common tail a/x/run.blf covers the whole old path; unclamped the roots would be "/" and "/b" and give
  // /b/dbc/v.dbc. Clamped they are /a and /b/a. Real root paths, so neither file exists: the rebased path is named.
  const AuxPathContext context{at("b/a/x/run.blf"), at("a/x/run.blf"), ""};
  EXPECT_EQ(resolveAuxPath(at("a/dbc/v.dbc"), context), at("b/a/dbc/v.dbc"));
}

TEST(DictionaryPaths, ResolveExternalAbsolutePathIsUntouched) {
  const AuxPathContext context{at("new/data.blf"), at("old/data.blf"), ""};
  EXPECT_EQ(resolveAuxPath(at("shared/v.dbc"), context), at("shared/v.dbc"));
}

TEST(DictionaryPaths, ResolveAbsolutePathWithoutSavedKeyIsUntouched) {
  const AuxPathContext context{at("new/data.blf"), "", at("old")};
  EXPECT_EQ(resolveAuxPath(at("old/v.dbc"), context), at("old/v.dbc"));
}

TEST(DictionaryPaths, ResolveSiblingWithSamePrefixIsNotRebased) {
  const AuxPathContext context{at("new/data.blf"), at("old/data.blf"), ""};
  EXPECT_EQ(resolveAuxPath(at("old2/v.dbc"), context), at("old2/v.dbc"));
}

TEST(DictionaryPaths, ResolveUrlIsUnchanged) {
  const AuxPathContext context{at("new/data.blf"), at("old/data.blf"), ""};
  EXPECT_EQ(resolveAuxPath("https://example.com/v.dbc", context), "https://example.com/v.dbc");
}

TEST(DictionaryPaths, ResolveWithoutADataFileIsUnchanged) {
  const AuxPathContext context{"", at("old/data.blf"), at("old")};
  EXPECT_EQ(resolveAuxPath("v.dbc", context), "v.dbc");
  EXPECT_EQ(resolveAuxPath(at("old/v.dbc"), context), at("old/v.dbc"));
}

TEST(DictionaryPaths, ResolveSiblingFolderFollowsAMove) {
  const AuxPathContext context{at("new/B/data/run.blf"), at("old/A/data/run.blf"), ""};
  EXPECT_EQ(resolveAuxPath(at("old/A/dbc/v.dbc"), context), at("new/B/dbc/v.dbc"));
  EXPECT_EQ(resolveAuxPath(at("old/A/data/v.dbc"), context), at("new/B/data/v.dbc"));
}

TEST(DictionaryPaths, ResolveDeeperCommonSuffixIsStripped) {
  const AuxPathContext context{at("y/proj2/logs/day1/run.blf"), at("x/proj/logs/day1/run.blf"), ""};
  EXPECT_EQ(resolveAuxPath(at("x/proj/dbc/v.dbc"), context), at("y/proj2/dbc/v.dbc"));
}

TEST(DictionaryPaths, ResolveRenamedDataFileFallsBackToParentDirs) {
  const AuxPathContext context{at("new/B/data/renamed.blf"), at("old/A/data/run.blf"), ""};
  EXPECT_EQ(resolveAuxPath(at("old/A/data/v.dbc"), context), at("new/B/data/v.dbc"));
  EXPECT_EQ(resolveAuxPath(at("old/A/dbc/v.dbc"), context), at("old/A/dbc/v.dbc"));
}

TEST(DictionaryPaths, ResolveDictionaryOutsideTheMovedRootStaysAbsolute) {
  const AuxPathContext context{at("new/B/data/run.blf"), at("old/A/data/run.blf"), ""};
  EXPECT_EQ(resolveAuxPath(at("old/other/v.dbc"), context), at("old/other/v.dbc"));
  EXPECT_EQ(resolveAuxPath(at("shared/v.dbc"), context), at("shared/v.dbc"));
}

TEST(DictionaryPaths, ResolveRootGuardFallsBackToParentDirs) {
  // Stripping the common "run.blf" would leave only the root, so the data directories are used instead.
  const AuxPathContext context{at("z/run.blf"), at("run.blf"), ""};
  EXPECT_EQ(resolveAuxPath(at("v.dbc"), context), at("z/v.dbc"));
  EXPECT_EQ(resolveAuxPath(at("dbc/v.dbc"), context), at("z/dbc/v.dbc"));
}

TEST(DictionaryPaths, SaveThenResolveRoundTripsAcrossAMove) {
  const std::string saved = toConfigPath(at("old/dbc/v.dbc"), at("old/data.blf"));
  ASSERT_EQ(saved, "dbc/v.dbc");
  const AuxPathContext context{at("new/data.blf"), at("old/data.blf"), auxBaseDir(at("old/data.blf"))};
  EXPECT_EQ(resolveAuxPath(saved, context), at("new/dbc/v.dbc"));
}

#ifdef _WIN32
TEST(DictionaryPaths, ResolveAcrossADriveChange) {
  const AuxPathContext context{"D:/new/data.blf", "C:/old/data.blf", ""};
  EXPECT_EQ(resolveAuxPath("C:/old/v.dbc", context), "D:/new/v.dbc");
  EXPECT_EQ(resolveAuxPath("C:/shared/v.dbc", context), "C:/shared/v.dbc");
}

TEST(DictionaryPaths, ResolveSiblingFolderAcrossADriveChange) {
  const AuxPathContext context{"D:/new/B/data/run.blf", "C:/old/A/data/run.blf", ""};
  EXPECT_EQ(resolveAuxPath("C:/old/A/dbc/v.dbc", context), "D:/new/B/dbc/v.dbc");
}
#endif

// ---------------------------------------------------------------------------
// vector helpers
// ---------------------------------------------------------------------------

TEST(DictionaryPaths, VectorHelpersMapEveryEntry) {
  const std::string data = at("run/data.log");
  EXPECT_EQ(
      toConfigPaths({at("run/a.dbc"), at("other/b.dbc")}, data),
      std::vector<std::string>({"a.dbc", at("other/b.dbc")}));
  const AuxPathContext context{data, "", ""};
  EXPECT_EQ(
      resolveAuxPaths({"a.dbc", at("other/b.dbc")}, context),
      std::vector<std::string>({at("run/a.dbc"), at("other/b.dbc")}));
  EXPECT_TRUE(toConfigPaths({}, data).empty());
}

// ---------------------------------------------------------------------------
// readAuxFile
// ---------------------------------------------------------------------------

TEST(DictionaryPaths, ReadAuxFileNamesAMissingFile) {
  ScratchDir scratch;
  const std::string path = generic(scratch.root() / "gone.dbc");
  const auto status = readAuxFile(path, "DBC");
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "DBC file not found: " + path);
}

TEST(DictionaryPaths, ReadAuxFileTreatsADirectoryAsNotFound) {
  ScratchDir scratch;
  const std::string path = generic(scratch.root());
  const auto status = readAuxFile(path, "dictionary");
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "dictionary file not found: " + path);
}

TEST(DictionaryPaths, ReadAuxFileReturnsTheWholeText) {
  ScratchDir scratch;
  const auto file = scratch.root() / "v.dbc";
  writeFile(file);
  const auto text = readAuxFile(generic(file), "DBC");
  ASSERT_TRUE(text);
  EXPECT_EQ(*text, "VERSION \"\"\n");
}

}  // namespace
