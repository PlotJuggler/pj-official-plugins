// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Drives the real runProcess() against tests/support/child_helper.cpp (a
// tiny standalone executable, ASSISTANT_CHILD_HELPER's full path) rather
// than mocking the OS -- the whole point of this file is that argv, stdin,
// the working directory and the environment survive the real POSIX
// fork/exec or Windows CreateProcess round trip exactly.
#include "subprocess.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "platform_util.hpp"  // utf8ToPath
#include "support/scoped_env.hpp"
#include "system_prompt.hpp"  // kSystemPrompt, used as one large ArgvSurvivesExactly entry

#ifndef ASSISTANT_CHILD_HELPER
#error "ASSISTANT_CHILD_HELPER must be defined by CMake"
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using assistant_agent::runProcess;
using assistant_agent::SubprocessResult;
using assistant_agent::testing::ScopedEnv;

std::string trimTrailingNewline(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
    s.pop_back();
  }
  return s;
}

// Parses child_helper's "argv" mode output ("<len>:<bytes>\n" per argument,
// byte-length-prefixed so an argument that itself contains a newline is
// still unambiguous) back into the original argument list.
bool parseArgvOutput(const std::string& out, std::vector<std::string>& result) {
  std::size_t pos = 0;
  while (pos < out.size()) {
    const std::size_t colon = out.find(':', pos);
    if (colon == std::string::npos) {
      return false;
    }
    std::size_t len = 0;
    for (std::size_t i = pos; i < colon; ++i) {
      if (out[i] < '0' || out[i] > '9') {
        return false;
      }
      len = len * 10 + static_cast<std::size_t>(out[i] - '0');
    }
    const std::size_t start = colon + 1;
    if (start + len >= out.size() || out[start + len] != '\n') {
      return false;
    }
    result.push_back(out.substr(start, len));
    pos = start + len + 1;
  }
  return true;
}

TEST(Subprocess, RoundTripsStdin) {
  std::string payload;
  payload.reserve(60 * 1024);
  while (payload.size() < 60 * 1024 - 16) {
    payload += "the quick brown fox jumps over the lazy dog ";
  }
  payload += "\xC3\xB1\xE2\x82\xAC\xF0\x9F\x98\x80";  // "ñ€😀" as raw UTF-8 bytes

  std::atomic<bool> no_cancel{false};
  std::string output;
  const SubprocessResult res = runProcess(
      {ASSISTANT_CHILD_HELPER, "cat"}, payload, [&](const std::string& chunk) { output += chunk; }, no_cancel);
  ASSERT_TRUE(res.spawned) << res.error;
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_EQ(output, payload);
}

TEST(Subprocess, ReportsExitCode) {
  std::atomic<bool> no_cancel{false};
  std::string output;
  const SubprocessResult res = runProcess(
      {ASSISTANT_CHILD_HELPER, "exit", "7"}, {}, [&](const std::string& chunk) { output += chunk; }, no_cancel);
  ASSERT_TRUE(res.spawned) << res.error;
  EXPECT_EQ(res.exit_code, 7);
}

// Every one of these survives a fork/exec with no shell involved (POSIX) or
// the appendQuotedArg round trip through a single command-line string
// (Windows) byte for byte -- no shell metacharacter is ever interpreted,
// and no quoting algorithm drops or duplicates a backslash.
TEST(Subprocess, ArgvSurvivesExactly) {
  const std::vector<std::string> inputs = {
      "",
      " ",
      "a\"b",
      "trail\\",
      "\\\\srv\\share\\",
      "\\\"",
      "\xC3\xB1\xE2\x82\xAC\xF0\x9F\x98\x80",  // "ñ€😀"
      "&whoami",
      "%PATH%",
      "^",
      std::string(assistant_agent::kSystemPrompt),
  };
  std::vector<std::string> argv = {ASSISTANT_CHILD_HELPER, "argv"};
  argv.insert(argv.end(), inputs.begin(), inputs.end());

  std::atomic<bool> no_cancel{false};
  std::string output;
  const SubprocessResult res = runProcess(argv, {}, [&](const std::string& chunk) { output += chunk; }, no_cancel);
  ASSERT_TRUE(res.spawned) << res.error;
  EXPECT_EQ(res.exit_code, 0);

  std::vector<std::string> got;
  ASSERT_TRUE(parseArgvOutput(output, got)) << "unparsable output: " << output;
  ASSERT_EQ(got.size(), inputs.size());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    EXPECT_EQ(got[i], inputs[i]) << "argument #" << i;
  }
}

TEST(Subprocess, HonoursWorkingDir) {
  namespace fs = std::filesystem;
  // "pj sub ñ" as raw UTF-8 bytes, spelled this way (rather than a literal
  // non-ASCII character in the source) so the test does not depend on this
  // file's own saved encoding.
  const fs::path dir = fs::temp_directory_path() / assistant_agent::utf8ToPath("pj sub \xC3\xB1");
  std::error_code ec;
  fs::create_directories(dir, ec);
  ASSERT_FALSE(ec) << ec.message();

  std::atomic<bool> no_cancel{false};
  std::string output;
  const SubprocessResult res = runProcess(
      {ASSISTANT_CHILD_HELPER, "cwd"}, {}, [&](const std::string& chunk) { output += chunk; }, no_cancel,
      assistant_agent::pathToUtf8(dir));
  ASSERT_TRUE(res.spawned) << res.error;
  EXPECT_EQ(res.exit_code, 0);

  const fs::path reported = assistant_agent::utf8ToPath(trimTrailingNewline(output));
  EXPECT_TRUE(fs::equivalent(reported, dir, ec)) << reported << " vs " << dir;

  fs::remove_all(dir, ec);
}

TEST(Subprocess, MissingWorkingDir) {
#if defined(_WIN32)
  const std::string missing_dir = "C:\\pj-assistant-test-missing\\definitely\\not\\here";
#else
  const std::string missing_dir = "/pj-assistant-test-missing/definitely/not/here";
#endif
  std::atomic<bool> no_cancel{false};
  std::string output;
  const SubprocessResult res = runProcess(
      {ASSISTANT_CHILD_HELPER, "cwd"}, {}, [&](const std::string& chunk) { output += chunk; }, no_cancel, missing_dir);
#if defined(_WIN32)
  EXPECT_FALSE(res.spawned);
#else
  ASSERT_TRUE(res.spawned) << res.error;
  EXPECT_EQ(res.exit_code, 127);
#endif
}

TEST(Subprocess, ExtraEnvAndInheritance) {
  ScopedEnv inherited("PJ_ASSISTANT_TEST_INHERITED", "inherited-value");
  std::atomic<bool> no_cancel{false};

  std::string extra_output;
  const SubprocessResult extra_res = runProcess(
      {ASSISTANT_CHILD_HELPER, "env", "PJ_ASSISTANT_TEST_EXTRA"}, {},
      [&](const std::string& chunk) { extra_output += chunk; }, no_cancel, /*working_dir=*/{},
      {{"PJ_ASSISTANT_TEST_EXTRA", "extra-value"}});
  ASSERT_TRUE(extra_res.spawned) << extra_res.error;
  EXPECT_EQ(trimTrailingNewline(extra_output), "extra-value");

  std::string inherited_output;
  const SubprocessResult inherited_res = runProcess(
      {ASSISTANT_CHILD_HELPER, "env", "PJ_ASSISTANT_TEST_INHERITED"}, {},
      [&](const std::string& chunk) { inherited_output += chunk; }, no_cancel);
  ASSERT_TRUE(inherited_res.spawned) << inherited_res.error;
  EXPECT_EQ(trimTrailingNewline(inherited_output), "inherited-value");
}

TEST(Subprocess, CancelKillsChild) {
  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    cancel.store(true);
  });

  const auto start = std::chrono::steady_clock::now();
  std::string output;
  const SubprocessResult res = runProcess(
      {ASSISTANT_CHILD_HELPER, "sleep", "60"}, {}, [&](const std::string& chunk) { output += chunk; }, cancel);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  canceller.join();

  EXPECT_TRUE(res.spawned) << res.error;
  EXPECT_LT(elapsed, std::chrono::seconds(5));
}

#if defined(_WIN32)

TEST(SubprocessWindows, EnvOverrideIsCaseInsensitive) {
  std::atomic<bool> no_cancel{false};
  std::string count_output;
  const SubprocessResult count_res = runProcess(
      {ASSISTANT_CHILD_HELPER, "env-count", "PATH"}, {}, [&](const std::string& chunk) { count_output += chunk; },
      no_cancel, /*working_dir=*/{}, {{"path", "X"}});
  ASSERT_TRUE(count_res.spawned) << count_res.error;
  EXPECT_EQ(trimTrailingNewline(count_output), "1");

  std::string value_output;
  const SubprocessResult value_res = runProcess(
      {ASSISTANT_CHILD_HELPER, "env", "PATH"}, {}, [&](const std::string& chunk) { value_output += chunk; }, no_cancel,
      /*working_dir=*/{}, {{"path", "X"}});
  ASSERT_TRUE(value_res.spawned) << value_res.error;
  EXPECT_EQ(trimTrailingNewline(value_output), "X");
}

TEST(SubprocessWindows, ChildExitDoesNotWaitForGrandchild) {
  std::atomic<bool> no_cancel{false};
  std::string output;
  const auto start = std::chrono::steady_clock::now();
  const SubprocessResult res = runProcess(
      {ASSISTANT_CHILD_HELPER, "spawn-grandchild"}, {}, [&](const std::string& chunk) { output += chunk; }, no_cancel);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(res.spawned) << res.error;
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_LT(elapsed, std::chrono::seconds(5));

  const DWORD grandchild_pid = static_cast<DWORD>(std::stoul(trimTrailingNewline(output)));
  // The job object kills every descendant when the immediate child exits
  // (subprocess.hpp), so the grandchild ("sleep 60") should already be gone.
  if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, grandchild_pid); h != nullptr) {
    EXPECT_EQ(WaitForSingleObject(h, 5000), WAIT_OBJECT_0);
    CloseHandle(h);
  }
}

TEST(SubprocessWindows, CancelKillsProcessTree) {
  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    cancel.store(true);
  });
  const auto start = std::chrono::steady_clock::now();
  std::string output;
  const SubprocessResult res = runProcess(
      {ASSISTANT_CHILD_HELPER, "spawn-grandchild"}, {}, [&](const std::string& chunk) { output += chunk; }, cancel);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  canceller.join();
  EXPECT_TRUE(res.spawned) << res.error;
  EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(SubprocessWindows, RefusesNonExe) {
  std::atomic<bool> no_cancel{false};

  const SubprocessResult cmd_res =
      runProcess({"C:\\Windows\\System32\\whoami.cmd"}, {}, [](const std::string&) {}, no_cancel);
  EXPECT_FALSE(cmd_res.spawned);

  const SubprocessResult relative_res = runProcess({"child_helper.exe"}, {}, [](const std::string&) {}, no_cancel);
  EXPECT_FALSE(relative_res.spawned);
}

#endif  // _WIN32

}  // namespace
