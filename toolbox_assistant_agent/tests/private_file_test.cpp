// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "private_file.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "platform_util.hpp"  // utf8ToPath

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace {

using assistant_agent::removePrivateTempFile;
using assistant_agent::writePrivateTempFile;

// A bare file-name prefix works on every platform: POSIX's writePrivateTempFile
// prepends "/tmp/" itself, and Windows writes under its own
// temp_directory_path() regardless — see private_file.hpp's contract comment.
constexpr const char* kPrefix = "pj_assistant_platform_test_";

std::string readWholeFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

TEST(PrivateFile, CreatesReadableFileWithExpectedContent) {
  std::string path;
  ASSERT_TRUE(writePrivateTempFile(kPrefix, "hello world", path));
  ASSERT_FALSE(path.empty());
  EXPECT_EQ(readWholeFile(path), "hello world");
  removePrivateTempFile(path);
}

TEST(PrivateFile, TwoCallsGiveDifferentPaths) {
  std::string a;
  std::string b;
  ASSERT_TRUE(writePrivateTempFile(kPrefix, "a", a));
  ASSERT_TRUE(writePrivateTempFile(kPrefix, "b", b));
  EXPECT_NE(a, b);
  removePrivateTempFile(a);
  removePrivateTempFile(b);
}

TEST(PrivateFile, RemoveDeletesTheFile) {
  std::string path;
  ASSERT_TRUE(writePrivateTempFile(kPrefix, "x", path));
  ASSERT_TRUE(std::filesystem::exists(assistant_agent::utf8ToPath(path)));
  removePrivateTempFile(path);
  EXPECT_FALSE(std::filesystem::exists(assistant_agent::utf8ToPath(path)));
}

TEST(PrivateFile, RemovingAnAlreadyGoneFileIsHarmless) {
  std::string path;
  ASSERT_TRUE(writePrivateTempFile(kPrefix, "x", path));
  removePrivateTempFile(path);
  removePrivateTempFile(path);  // must not throw or crash on a second call
}

TEST(PrivateFile, NameStartsWithThePrefixsFilename) {
  std::string path;
  ASSERT_TRUE(writePrivateTempFile(kPrefix, "z", path));
  const std::string filename = assistant_agent::utf8ToPath(path).filename().string();
  EXPECT_EQ(filename.rfind("pj_assistant_platform_test_", 0), 0u);
  removePrivateTempFile(path);
}

#if defined(__unix__) || defined(__APPLE__)

TEST(PrivateFilePosix, PathUnderTmpWithMode0600) {
  std::string path;
  ASSERT_TRUE(writePrivateTempFile(kPrefix, "content", path));
  EXPECT_EQ(path.rfind("/tmp/", 0), 0u);

  struct stat st{};
  ASSERT_EQ(stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);

  removePrivateTempFile(path);
}

#elif defined(_WIN32)

TEST(PrivateFileWindows, ParentEqualsTempDirectoryPath) {
  std::string path;
  ASSERT_TRUE(writePrivateTempFile(kPrefix, "content", path));

  std::error_code ec;
  const std::filesystem::path expected = std::filesystem::temp_directory_path(ec);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_TRUE(std::filesystem::equivalent(assistant_agent::utf8ToPath(path).parent_path(), expected, ec));

  removePrivateTempFile(path);
}

#endif

}  // namespace
