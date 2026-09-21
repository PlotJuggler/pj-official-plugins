// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// locateCli()/childEnvFor() against real temp directories and a real HOME/PATH
// (via ScopedEnv) — the fallback order, the nvm semver comparison, and the
// one case that demonstrates why resolving a path is not enough on its own:
// an `#!/usr/bin/env` shebang whose interpreter lives next to the script but
// is not on this process's PATH. Below that: shared tests that run on every
// platform (detail::splitPath, cannotFindCliMessage's npm_shim wording), then
// a POSIX-only block and a Windows-only block, each covering that platform's
// own search order and quoting/refusal rules.
#include "cli_locator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "cli_probe.hpp"  // cannotFindCliMessage
#include "subprocess.hpp"
#include "support/scoped_env.hpp"

// --- Shared: platform-neutral pieces, exercised on every platform ---------

namespace {

TEST(CliLocatorSplitPath, PosixColonSeparator) {
  const auto parts = assistant_agent::detail::splitPath("/a/b:/c/d:", ':');
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "/a/b");
  EXPECT_EQ(parts[1], "/c/d");
  EXPECT_EQ(parts[2], "");
}

TEST(CliLocatorSplitPath, WindowsSemicolonSeparator) {
  const auto parts = assistant_agent::detail::splitPath(R"(C:\a;C:\b;)", ';');
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], R"(C:\a)");
  EXPECT_EQ(parts[1], R"(C:\b)");
  EXPECT_EQ(parts[2], "");
}

TEST(CliLocatorSplitPath, EmptyStringYieldsOneEmptyEntry) {
  const auto parts = assistant_agent::detail::splitPath("", ':');
  ASSERT_EQ(parts.size(), 1u);
  EXPECT_EQ(parts[0], "");
}

// cannotFindCliMessage only reads CliLocation fields, so its npm_shim
// wording is testable here even though locateCli() itself only ever fills
// npm_shim on Windows.
TEST(CannotFindCliMessage, PlainMessageWhenNoShimWasFound) {
  assistant_agent::CliLocation loc;
  loc.searched = {"/a/b", "/c/d"};
  const std::string msg = assistant_agent::cannotFindCliMessage("codex", loc);
  EXPECT_NE(msg.find("/a/b"), std::string::npos);
  EXPECT_NE(msg.find("/c/d"), std::string::npos);
  EXPECT_NE(msg.find("Set the CLI path in Settings"), std::string::npos);
  EXPECT_EQ(msg.find("npm launcher"), std::string::npos);
}

TEST(CannotFindCliMessage, NamesTheNpmShimAndTheNativeClaudeInstaller) {
  assistant_agent::CliLocation loc;
  loc.searched = {R"(C:\a)"};
  loc.npm_shim = R"(C:\a\claude.cmd)";
  const std::string msg = assistant_agent::cannotFindCliMessage("claude", loc);
  EXPECT_NE(msg.find(R"(C:\a\claude.cmd)"), std::string::npos);
  EXPECT_NE(msg.find("npm launcher"), std::string::npos);
  EXPECT_NE(msg.find("claude.ai/install.ps1"), std::string::npos);
}

TEST(CannotFindCliMessage, NamesTheNativeCodexInstallerForAFullPath) {
  assistant_agent::CliLocation loc;
  loc.npm_shim = R"(C:\a\codex.cmd)";
  const std::string msg = assistant_agent::cannotFindCliMessage(R"(C:\a\codex)", loc);
  EXPECT_NE(msg.find("chatgpt.com/codex/install.ps1"), std::string::npos);
}

}  // namespace

#if defined(__unix__) || defined(__APPLE__)

namespace {

using assistant_agent::childEnvFor;
using assistant_agent::CliLocation;
using assistant_agent::locateCli;
using assistant_agent::runProcess;
using assistant_agent::testing::makeTempDir;
using assistant_agent::testing::ScopedEnv;

namespace fs = std::filesystem;

bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

// Writes an executable text file at `dir/name` with `contents`.
void writeExecutable(const fs::path& dir, const std::string& name, const std::string& contents) {
  const fs::path path = dir / name;
  {
    std::ofstream out(path);
    out << contents;
  }
  chmod(path.c_str(), 0700);
}

class CliLocatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    home_ = makeTempDir("pj_cli_locator_home_");
    ASSERT_FALSE(home_.empty());
    empty_path_dir_ = makeTempDir("pj_cli_locator_emptypath_");
    ASSERT_FALSE(empty_path_dir_.empty());
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(home_, ec);
    fs::remove_all(empty_path_dir_, ec);
  }

  fs::path home_;
  fs::path empty_path_dir_;
};

TEST_F(CliLocatorTest, PathEntryWinsOverFallbacks) {
  const fs::path path_dir = makeTempDir("pj_cli_locator_pathdir_");
  ASSERT_FALSE(path_dir.empty());
  writeExecutable(path_dir, "codex", "#!/bin/sh\necho from-path\n");

  const fs::path local_bin = home_ / ".local" / "bin";
  fs::create_directories(local_bin);
  writeExecutable(local_bin, "codex", "#!/bin/sh\necho from-local-bin\n");

  ScopedEnv home_env("HOME", home_);
  ScopedEnv path_env("PATH", path_dir);

  const CliLocation loc = locateCli("codex");
  EXPECT_EQ(loc.path, (path_dir / "codex").string());
  EXPECT_EQ(loc.bin_dir, path_dir.string());

  std::error_code ec;
  fs::remove_all(path_dir, ec);
}

TEST_F(CliLocatorTest, FallsBackToLocalBin) {
  const fs::path local_bin = home_ / ".local" / "bin";
  fs::create_directories(local_bin);
  writeExecutable(local_bin, "codex", "#!/bin/sh\necho from-local-bin\n");

  ScopedEnv home_env("HOME", home_);
  ScopedEnv path_env("PATH", empty_path_dir_);

  const CliLocation loc = locateCli("codex");
  EXPECT_EQ(loc.path, (local_bin / "codex").string());
  EXPECT_EQ(loc.bin_dir, local_bin.string());
}

TEST_F(CliLocatorTest, NvmDefaultAliasWins) {
  const fs::path v9 = home_ / ".nvm" / "versions" / "node" / "v9.0.0" / "bin";
  const fs::path v22 = home_ / ".nvm" / "versions" / "node" / "v22.0.0" / "bin";
  fs::create_directories(v9);
  fs::create_directories(v22);
  writeExecutable(v9, "codex", "#!/bin/sh\necho v9\n");
  writeExecutable(v22, "codex", "#!/bin/sh\necho v22\n");

  const fs::path alias_dir = home_ / ".nvm" / "alias";
  fs::create_directories(alias_dir);
  {
    std::ofstream alias(alias_dir / "default");
    alias << "v9.0.0\n";
  }

  ScopedEnv home_env("HOME", home_);
  ScopedEnv path_env("PATH", empty_path_dir_);

  const CliLocation loc = locateCli("codex");
  EXPECT_EQ(loc.path, (v9 / "codex").string()) << "alias/default names v9.0.0 explicitly; it must win "
                                                  "even though v22.0.0 is the newer install";
}

TEST_F(CliLocatorTest, NvmHighestVersionWithoutAlias) {
  const fs::path v9 = home_ / ".nvm" / "versions" / "node" / "v9.0.0" / "bin";
  const fs::path v22 = home_ / ".nvm" / "versions" / "node" / "v22.0.0" / "bin";
  fs::create_directories(v9);
  fs::create_directories(v22);
  writeExecutable(v9, "codex", "#!/bin/sh\necho v9\n");
  writeExecutable(v22, "codex", "#!/bin/sh\necho v22\n");
  // No alias/default at all.

  ScopedEnv home_env("HOME", home_);
  ScopedEnv path_env("PATH", empty_path_dir_);

  const CliLocation loc = locateCli("codex");
  EXPECT_EQ(loc.path, (v22 / "codex").string())
      << "without an alias, the highest version must win by NUMERIC comparison: a lexicographic "
         "compare would put v9 ahead of v22";
}

TEST_F(CliLocatorTest, ConfiguredAbsolutePathAndTilde) {
  const fs::path tools_dir = home_ / "tools";
  fs::create_directories(tools_dir);
  writeExecutable(tools_dir, "codex", "#!/bin/sh\necho tools\n");

  ScopedEnv home_env("HOME", home_);

  const CliLocation found = locateCli("~/tools/codex");
  EXPECT_EQ(found.path, (tools_dir / "codex").string());
  EXPECT_EQ(found.bin_dir, tools_dir.string());

  const CliLocation missing = locateCli("~/tools/does-not-exist");
  EXPECT_TRUE(missing.path.empty());
  ASSERT_EQ(missing.searched.size(), 1u);
  EXPECT_EQ(missing.searched[0], (home_ / "tools" / "does-not-exist").string());
}

TEST_F(CliLocatorTest, NotFoundListsEverySearchedDir) {
  ScopedEnv home_env("HOME", home_);
  ScopedEnv path_env("PATH", empty_path_dir_);

  const CliLocation loc = locateCli("codex-that-does-not-exist-anywhere");
  EXPECT_TRUE(loc.path.empty());
  EXPECT_TRUE(loc.bin_dir.empty());
  EXPECT_TRUE(contains(loc.searched, empty_path_dir_.string()));
  EXPECT_TRUE(contains(loc.searched, (home_ / ".local" / "bin").string()));
  EXPECT_TRUE(contains(loc.searched, (home_ / ".npm-global" / "bin").string()));
  EXPECT_TRUE(contains(loc.searched, (home_ / ".volta" / "bin").string()));
  EXPECT_TRUE(contains(loc.searched, std::string("/usr/local/bin")));
  EXPECT_TRUE(contains(loc.searched, std::string("/opt/homebrew/bin")));
  EXPECT_TRUE(contains(loc.searched, std::string("/home/linuxbrew/.linuxbrew/bin")));
}

// The case that demonstrates why resolving a path is not enough on its own:
// `codex` here is `#!/usr/bin/env fakenode`, and `fakenode` lives right next
// to it but nowhere on this process's own PATH. Only childEnvFor(loc), which
// prepends that directory to the CHILD's PATH, lets `env` find it.
TEST_F(CliLocatorTest, ChildPathLetsAnEnvShebangResolve) {
  const fs::path bin_dir = home_ / ".local" / "bin";
  fs::create_directories(bin_dir);
  writeExecutable(bin_dir, "codex", "#!/usr/bin/env fakenode\n");
  writeExecutable(bin_dir, "fakenode", "#!/bin/sh\necho ok\n");

  ScopedEnv home_env("HOME", home_);
  ScopedEnv path_env("PATH", empty_path_dir_);

  const CliLocation loc = locateCli("codex");
  ASSERT_FALSE(loc.path.empty());
  EXPECT_EQ(loc.bin_dir, bin_dir.string());

  std::atomic<bool> no_cancel{false};
  std::string output_with_env;
  auto with_env = runProcess(
      {loc.path}, /*stdin_data=*/{}, [&](const std::string& chunk) { output_with_env += chunk; }, no_cancel,
      /*working_dir=*/{}, childEnvFor(loc));
  EXPECT_TRUE(with_env.spawned);
  EXPECT_EQ(with_env.exit_code, 0);
  EXPECT_NE(output_with_env.find("ok"), std::string::npos);

  std::string output_without_env;
  auto without_env = runProcess(
      {loc.path}, /*stdin_data=*/{}, [&](const std::string& chunk) { output_without_env += chunk; }, no_cancel);
  EXPECT_TRUE(without_env.spawned);
  EXPECT_NE(without_env.exit_code, 0) << "without the child PATH prepend, `env fakenode` cannot resolve";
}

}  // namespace

#elif defined(_WIN32)

namespace {

using assistant_agent::childEnvFor;
using assistant_agent::CliLocation;
using assistant_agent::locateCli;
using assistant_agent::testing::makeTempDir;
using assistant_agent::testing::ScopedEnv;

namespace fs = std::filesystem;

// isExecutableFile() on Windows only checks the extension and that the path
// is a regular file (there is no POSIX execute bit to set), so an empty file
// with the right name and extension is enough of a fixture — see
// cli_locator.hpp's detail::isExecutableFile.
void writeEmptyFile(const fs::path& path) {
  std::ofstream(path).close();
}

class CliLocatorWindowsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    home_ = makeTempDir("pj_cli_locator_home_");
    ASSERT_FALSE(home_.empty());
    empty_path_dir_ = makeTempDir("pj_cli_locator_emptypath_");
    ASSERT_FALSE(empty_path_dir_.empty());
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(home_, ec);
    fs::remove_all(empty_path_dir_, ec);
  }

  fs::path home_;
  fs::path empty_path_dir_;
};

TEST_F(CliLocatorWindowsTest, FindsExeOnPath) {
  const fs::path path_dir = makeTempDir("pj_cli_locator_pathdir_");
  ASSERT_FALSE(path_dir.empty());
  writeEmptyFile(path_dir / "codex.exe");

  ScopedEnv home_env("HOME", home_);
  ScopedEnv path_env("PATH", path_dir);

  const CliLocation loc = locateCli("codex");
  EXPECT_EQ(loc.path, (path_dir / "codex.exe").string());
  EXPECT_EQ(loc.bin_dir, path_dir.string());

  std::error_code ec;
  fs::remove_all(path_dir, ec);
}

TEST_F(CliLocatorWindowsTest, QuotedPathEntryIsStripped) {
  const fs::path path_dir = makeTempDir("pj_cli_locator_quoted_");
  ASSERT_FALSE(path_dir.empty());
  writeEmptyFile(path_dir / "codex.exe");

  ScopedEnv home_env("HOME", home_);
  const std::string quoted_path = "\"" + path_dir.string() + "\"";
  ScopedEnv path_env("PATH", quoted_path.c_str());

  const CliLocation loc = locateCli("codex");
  EXPECT_EQ(loc.path, (path_dir / "codex.exe").string());

  std::error_code ec;
  fs::remove_all(path_dir, ec);
}

TEST_F(CliLocatorWindowsTest, BareNameAlreadyEndingInExeIsNotDoubled) {
  const fs::path path_dir = makeTempDir("pj_cli_locator_exe_");
  ASSERT_FALSE(path_dir.empty());
  writeEmptyFile(path_dir / "codex.exe");

  ScopedEnv home_env("HOME", home_);
  ScopedEnv path_env("PATH", path_dir);

  const CliLocation loc = locateCli("codex.exe");
  EXPECT_EQ(loc.path, (path_dir / "codex.exe").string()) << "must not become codex.exe.exe";

  std::error_code ec;
  fs::remove_all(path_dir, ec);
}

TEST_F(CliLocatorWindowsTest, FallsBackToUserProfileLocalBin) {
  const fs::path local_bin = home_ / ".local" / "bin";
  fs::create_directories(local_bin);
  writeEmptyFile(local_bin / "codex.exe");

  ScopedEnv home_env("HOME", nullptr);
  ScopedEnv userprofile_env("USERPROFILE", home_);
  ScopedEnv path_env("PATH", empty_path_dir_);

  const CliLocation loc = locateCli("codex");
  EXPECT_EQ(loc.path, (local_bin / "codex.exe").string());
  EXPECT_EQ(loc.bin_dir, local_bin.string());
}

TEST_F(CliLocatorWindowsTest, ExplicitBackslashPath) {
  const fs::path tools_dir = home_ / "tools";
  fs::create_directories(tools_dir);
  writeEmptyFile(tools_dir / "codex.exe");

  const std::string configured = (tools_dir / "codex.exe").string();
  const CliLocation loc = locateCli(configured);
  EXPECT_EQ(loc.path, configured);
  EXPECT_EQ(loc.bin_dir, tools_dir.string());
}

TEST_F(CliLocatorWindowsTest, CmdOnlySetsNpmShimAndLeavesPathEmpty) {
  const fs::path path_dir = makeTempDir("pj_cli_locator_cmdonly_");
  ASSERT_FALSE(path_dir.empty());
  writeEmptyFile(path_dir / "codex.cmd");

  ScopedEnv home_env("HOME", home_);
  ScopedEnv path_env("PATH", path_dir);

  const CliLocation loc = locateCli("codex");
  EXPECT_TRUE(loc.path.empty());
  EXPECT_EQ(loc.npm_shim, (path_dir / "codex.cmd").string());

  std::error_code ec;
  fs::remove_all(path_dir, ec);
}

TEST_F(CliLocatorWindowsTest, ExplicitCmdPathIsRefusedButNamedAsShim) {
  const fs::path path_dir = makeTempDir("pj_cli_locator_explicitcmd_");
  ASSERT_FALSE(path_dir.empty());
  writeEmptyFile(path_dir / "codex.cmd");

  const std::string configured = (path_dir / "codex.cmd").string();
  const CliLocation loc = locateCli(configured);
  EXPECT_TRUE(loc.path.empty()) << "a .cmd is never run directly, see subprocess.hpp's Windows contract";
  EXPECT_EQ(loc.npm_shim, configured);

  std::error_code ec;
  fs::remove_all(path_dir, ec);
}

TEST_F(CliLocatorWindowsTest, ChildEnvForUsesSemicolon) {
  CliLocation loc;
  loc.bin_dir = R"(C:\tools)";
  ScopedEnv path_env("PATH", R"(C:\existing)");

  const auto env = childEnvFor(loc);
  ASSERT_EQ(env.size(), 1u);
  EXPECT_EQ(env[0].first, "PATH");
  EXPECT_EQ(env[0].second, R"(C:\tools;C:\existing)");
}

}  // namespace

#endif  // POSIX / Windows
