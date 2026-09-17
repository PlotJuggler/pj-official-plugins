// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// locateCli()/childEnvFor() against real temp directories and a real HOME/PATH
// (via ScopedEnv) — the fallback order, the nvm semver comparison, and the
// one case that demonstrates why resolving a path is not enough on its own:
// an `#!/usr/bin/env` shebang whose interpreter lives next to the script but
// is not on this process's PATH.
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

#include "subprocess.hpp"
#include "support/scoped_env.hpp"

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

#endif  // POSIX
