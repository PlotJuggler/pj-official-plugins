// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// A standalone helper process spawned by subprocess_test.cpp, cli_locator_test.cpp
// and the fake-CLI backend tests (via backend_test_helpers.hpp's
// writeFakeCliScript) — never linked into any of this plugin's own binaries,
// so it is deliberately dependency-free (no gtest, no plotjuggler_sdk): just
// the C++ standard library plus, on Windows, kernel32/msvcrt for the
// UTF-8/UTF-16 conversion and the console-mode calls no standard header
// covers. Selects a mode from argv[1], EXCEPT that a "fake CLI" mode
// (writeFakeCliScript's own use) takes over first, before any mode dispatch —
// see runFakeCliIfScripted below.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>

// Windows-only: on this platform platform_util.hpp includes only <windows.h>
// plus std headers (its POSIX branch is the one that needs pj_base), so
// reusing its utf8ToWide/wideToUtf8 here does not add an SDK dependency to
// this deliberately dependency-free helper. src/ is on this target's include
// path via CMakeLists.txt.
#include "platform_util.hpp"  // assistant_agent::detail::utf8ToWide/wideToUtf8
#else
#include <unistd.h>
#endif

namespace {

// If `<own_exe_path>.script` exists, this process IS a fake CLI: drain stdin
// (so a payload writer never blocks/SIGPIPEs on an unread pipe), then read
// the script -- line 1 is the exit code, every remaining line is printed
// verbatim to stdout -- and exit with it. Returns -1 (and does nothing else)
// when no such file exists, the signal to fall through to ordinary mode
// dispatch.
int runFakeCliIfScripted(const std::string& own_exe_path) {
  std::ifstream script(own_exe_path + ".script");
  if (!script) {
    return -1;
  }
  char discard[4096];
  while (std::cin.read(discard, sizeof(discard)) || std::cin.gcount() > 0) {}

  int exit_code = 0;
  bool first_line = true;
  std::string line;
  while (std::getline(script, line)) {
    if (first_line) {
      exit_code = std::atoi(line.c_str());
      first_line = false;
    } else {
      std::cout << line << "\n";
    }
  }
  std::cout.flush();
  return exit_code;
}

int modeCat() {
  char buf[4096];
  while (std::cin.read(buf, sizeof(buf)) || std::cin.gcount() > 0) {
    std::cout.write(buf, std::cin.gcount());
  }
  std::cout.flush();
  return 0;
}

int modeExit(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    return 1;
  }
  return std::atoi(args[2].c_str());
}

int modeArgv(const std::vector<std::string>& args) {
  for (std::size_t i = 2; i < args.size(); ++i) {
    std::cout << args[i].size() << ":" << args[i] << "\n";
  }
  std::cout.flush();
  return 0;
}

int modeCwd() {
#if defined(_WIN32)
  wchar_t buf[32768];
  const DWORD n = GetCurrentDirectoryW(32768, buf);
  if (n > 0 && n < 32768) {
    std::cout << assistant_agent::detail::wideToUtf8(std::wstring(buf, n)) << "\n";
  }
#else
  char buf[8192];
  if (getcwd(buf, sizeof(buf)) != nullptr) {
    std::cout << buf << "\n";
  }
#endif
  std::cout.flush();
  return 0;
}

int modeEnv(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    return 1;
  }
  const char* value = std::getenv(args[2].c_str());
  std::cout << (value != nullptr ? value : "<unset>") << "\n";
  std::cout.flush();
  return 0;
}

#if defined(_WIN32)
int modeEnvCount(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    return 1;
  }
  const std::wstring wname = assistant_agent::detail::utf8ToWide(args[2]);
  int count = 0;
  if (LPWCH env = GetEnvironmentStringsW(); env != nullptr) {
    const wchar_t* p = env;
    while (*p != L'\0') {
      const std::wstring line(p);
      const std::size_t search_from = (!line.empty() && line.front() == L'=') ? 1u : 0u;
      const std::size_t eq = line.find(L'=', search_from);
      const std::wstring name = line.substr(0, eq == std::wstring::npos ? line.size() : eq);
      if (CompareStringOrdinal(name.c_str(), -1, wname.c_str(), -1, TRUE) == CSTR_EQUAL) {
        ++count;
      }
      p += line.size() + 1;
    }
    FreeEnvironmentStringsW(env);
  }
  std::cout << count << "\n";
  std::cout.flush();
  return 0;
}
#endif

int modeSleep(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::duration<double>(std::atof(args[2].c_str())));
  return 0;
}

#if defined(_WIN32)
// Spawns a grandchild (this same exe, "sleep 60") that inherits this
// process's stdio, prints its PID, then exits -- so a test can check that
// killing the immediate child does not leave a grandchild holding the
// output pipe open (see subprocess.hpp's job-object handling).
int modeSpawnGrandchild(const std::string& own_exe_path) {
  const std::wstring app = assistant_agent::detail::utf8ToWide(own_exe_path);
  std::wstring cmdline = L"\"" + app + L"\" sleep 60";
  std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
  cmdline_buf.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(
          app.c_str(), cmdline_buf.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE, 0, nullptr, nullptr, &si, &pi)) {
    return 1;
  }
  std::cout << pi.dwProcessId << "\n";
  std::cout.flush();
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 0;
}
#endif

int run(const std::vector<std::string>& args) {
  if (args.empty()) {
    return 1;
  }
  if (const int fake_cli_exit = runFakeCliIfScripted(args[0]); fake_cli_exit != -1) {
    return fake_cli_exit;
  }
  if (args.size() < 2) {
    std::cerr << "usage: child_helper <mode> [args...]\n";
    return 1;
  }
  const std::string& mode = args[1];
  if (mode == "cat") {
    return modeCat();
  }
  if (mode == "exit") {
    return modeExit(args);
  }
  if (mode == "argv") {
    return modeArgv(args);
  }
  if (mode == "cwd") {
    return modeCwd();
  }
  if (mode == "env") {
    return modeEnv(args);
  }
  if (mode == "sleep") {
    return modeSleep(args);
  }
#if defined(_WIN32)
  if (mode == "env-count") {
    return modeEnvCount(args);
  }
  if (mode == "spawn-grandchild") {
    return modeSpawnGrandchild(args[0]);
  }
#endif
  std::cerr << "unknown mode: " << mode << "\n";
  return 1;
}

}  // namespace

#if defined(_WIN32)

int wmain(int argc, wchar_t** argv) {
  // Binary mode: the text-mode CRLF<->LF translation Windows applies by
  // default would corrupt an exact byte round trip (RoundTripsStdin,
  // ArgvSurvivesExactly) the moment a 0x0D byte appears anywhere in the data.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);

  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.push_back(assistant_agent::detail::wideToUtf8(argv[i]));
  }
  return run(args);
}

#else

int main(int argc, char** argv) {
  return run(std::vector<std::string>(argv, argv + argc));
}

#endif
