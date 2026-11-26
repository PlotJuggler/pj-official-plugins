// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace assistant_agent {

struct SubprocessResult {
  bool spawned = false;
  int exit_code = -1;
  std::string error;
};

// Runs argv[0] with argv, streaming stdout to `on_stdout` in raw chunks (the caller splits
// lines). argv goes straight to execvp: NO shell, so nothing forwarded can inject commands.
// `stdin_data` is written in full to the child's stdin, then closed, before stdout is
// drained, so keep it below the pipe buffer (~64 KiB); empty gives a closed stdin. Blocks
// until the child exits; if `cancel` becomes true the child gets SIGTERM and is reaped.
// stderr is discarded on every platform.
//
// A non-empty `working_dir` is the child's cwd (chdir after fork, before exec; the child
// exits 127 if it cannot enter it). Pass a neutral directory for tools that derive context
// from cwd (the Claude CLI reads its CLAUDE.md and project state).
//
// `extra_env` is applied with setenv() in the child on top of the inherited environment,
// never replacing it. It carries the MCP bearer token to Codex, keeping it off argv
// (readable via /proc/<pid>/cmdline).
//
// POSIX and Windows; other platforms return spawned=false. Windows differs from the above:
//   - a missing `working_dir` fails the call (spawned=false) instead of the child exiting
//     127: CreateProcessW rejects a bad cwd before the child starts;
//   - `cancel` kills the whole process tree (a job object), not one process;
//   - argv[0] must be an absolute path to a native `.exe`: a `.bat`/`.cmd` routes through
//     cmd.exe, whose parsing is the "BatBadBut" injection class;
//   - once the child exits, its descendants are killed via the job object and stdout is
//     drained to EOF, so a grandchild's inherited handle cannot hold the pipe open.
SubprocessResult runProcess(
    const std::vector<std::string>& argv, const std::string& stdin_data,
    const std::function<void(const std::string&)>& on_stdout, const std::atomic<bool>& cancel,
    const std::string& working_dir = {}, const std::vector<std::pair<std::string, std::string>>& extra_env = {});

}  // namespace assistant_agent

#if defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace assistant_agent {

inline SubprocessResult runProcess(
    const std::vector<std::string>& argv, const std::string& stdin_data,
    const std::function<void(const std::string&)>& on_stdout, const std::atomic<bool>& cancel,
    const std::string& working_dir, const std::vector<std::pair<std::string, std::string>>& extra_env) {
  if (argv.empty()) {
    return {false, -1, "empty argv"};
  }
  int fds[2];
  if (pipe(fds) != 0) {
    return {false, -1, "pipe() failed"};
  }
  int in_fds[2];
  if (pipe(in_fds) != 0) {
    close(fds[0]);
    close(fds[1]);
    return {false, -1, "pipe() failed"};
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    close(in_fds[0]);
    close(in_fds[1]);
    return {false, -1, "fork() failed"};
  }
  if (pid == 0) {
    dup2(in_fds[0], STDIN_FILENO);
    dup2(fds[1], STDOUT_FILENO);
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
    }
    close(in_fds[0]);
    close(in_fds[1]);
    close(fds[0]);
    close(fds[1]);
    if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) {
      _exit(127);  // never run in the wrong directory
    }
    for (const auto& [name, value] : extra_env) {
      setenv(name.c_str(), value.c_str(), 1);
    }
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) {
      cargv.push_back(const_cast<char*>(a.c_str()));
    }
    cargv.push_back(nullptr);
    execvp(cargv[0], cargv.data());
    _exit(127);  // exec failed
  }

  // Parent: stdin goes up front (chat-sized, fits the pipe buffer), then EOF, then drain stdout.
  close(in_fds[0]);
  if (!stdin_data.empty()) {
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(stdin_data.size())) {
      const ssize_t w = write(in_fds[1], stdin_data.data() + off, stdin_data.size() - static_cast<std::size_t>(off));
      if (w <= 0) {
        break;
      }
      off += w;
    }
  }
  close(in_fds[1]);
  close(fds[1]);
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  bool killed = false;
  bool eof = false;
  char buf[4096];
  while (!eof) {
    if (cancel.load() && !killed) {
      kill(pid, SIGTERM);
      killed = true;
    }
    struct pollfd pfd{fds[0], POLLIN, 0};
    const int pr = poll(&pfd, 1, 200);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (pr == 0) {
      continue;  // timeout: re-check cancel
    }
    if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
      const ssize_t n = read(fds[0], buf, sizeof(buf));
      if (n > 0) {
        on_stdout(std::string(buf, static_cast<std::size_t>(n)));
      } else if (n == 0) {
        eof = true;
      } else if (errno != EAGAIN && errno != EINTR) {
        eof = true;
      }
    }
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {true, code, ""};
}

}  // namespace assistant_agent

#elif defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "platform_util.hpp"  // utf8ToWide/wideToUtf8 (detail), utf8ToPath, detail::endsWithCi

namespace assistant_agent {

namespace detail {

// Move-only owner of a Win32 HANDLE. Both null and INVALID_HANDLE_VALUE mean "nothing to
// close": CreateFileW returns the latter on failure, every other API here the former.
struct ScopedHandle {
  HANDLE h = nullptr;

  ScopedHandle() = default;
  explicit ScopedHandle(HANDLE handle) : h(handle) {}
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&& other) noexcept : h(other.release()) {}
  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  ~ScopedHandle() {
    reset();
  }

  void reset(HANDLE new_h = nullptr) {
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
    }
    h = new_h;
  }

  [[nodiscard]] HANDLE release() {
    HANDLE tmp = h;
    h = nullptr;
    return tmp;
  }

  explicit operator bool() const {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
  }
};

// Appends one argument to `cmd`, quoted so the child's re-split (the CRT startup /
// CommandLineToArgvW algorithm) reproduces it byte for byte. A backslash doubles only right
// before a quote character or the argument's own closing quote; doubling it elsewhere would
// corrupt a path like `C:\Users\x\file`.
inline void appendQuotedArg(std::wstring& cmd, const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd += a;
    return;
  }
  cmd += L'"';
  for (auto it = a.begin();; ++it) {
    std::size_t bs = 0;
    while (it != a.end() && *it == L'\\') {
      ++it;
      ++bs;
    }
    if (it == a.end()) {
      cmd.append(bs * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      cmd.append(bs * 2 + 1, L'\\');
    } else {
      cmd.append(bs, L'\\');
    }
    cmd += *it;
  }
  cmd += L'"';
}

// Builds the double-NUL-terminated environment block CreateProcessW expects: the inherited
// entries with `extra_env` on top (a case-insensitive name match replaces EVERY such entry,
// leaving no shadowed duplicate; no match appends), sorted case-insensitively by name as a
// well-formed block is. A leading '=' entry (per-drive hidden cwd, e.g. "=C:=C:\x") is kept
// verbatim: its NAME runs to the SECOND '='.
inline std::wstring buildEnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& extra_env) {
  struct Entry {
    std::wstring name;
    std::wstring line;
  };
  std::vector<Entry> entries;

  if (LPWCH raw = GetEnvironmentStringsW(); raw != nullptr) {
    const wchar_t* p = raw;
    while (*p != L'\0') {
      const std::wstring line(p);
      const std::size_t search_from = (!line.empty() && line.front() == L'=') ? 1u : 0u;
      const std::size_t eq = line.find(L'=', search_from);
      entries.push_back({line.substr(0, eq == std::wstring::npos ? line.size() : eq), line});
      p += line.size() + 1;
    }
    FreeEnvironmentStringsW(raw);
  }

  for (const auto& [name_utf8, value_utf8] : extra_env) {
    const std::wstring wname = utf8ToWide(name_utf8);
    const std::wstring wline = wname + L'=' + utf8ToWide(value_utf8);
    // The first case-insensitive match becomes the replacement and further matches are
    // erased, so no duplicate "NAME=value" lines remain.
    bool replaced = false;
    for (auto it = entries.begin(); it != entries.end();) {
      if (CompareStringOrdinal(it->name.c_str(), -1, wname.c_str(), -1, TRUE) == CSTR_EQUAL) {
        if (!replaced) {
          it->name = wname;
          it->line = wline;
          replaced = true;
          ++it;
        } else {
          it = entries.erase(it);
        }
      } else {
        ++it;
      }
    }
    if (!replaced) {
      entries.push_back({wname, wline});
    }
  }

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });

  std::wstring block;
  for (const Entry& e : entries) {
    block += e.line;
    block += L'\0';
  }
  block += L'\0';  // the block's terminating NUL
  return block;
}

}  // namespace detail

inline SubprocessResult runProcess(
    const std::vector<std::string>& argv, const std::string& stdin_data,
    const std::function<void(const std::string&)>& on_stdout, const std::atomic<bool>& cancel,
    const std::string& working_dir, const std::vector<std::pair<std::string, std::string>>& extra_env) {
  if (argv.empty()) {
    return {false, -1, "empty argv"};
  }

  // See the Windows notes in the contract above.
  const std::filesystem::path exe_path = utf8ToPath(argv[0]);
  if (!exe_path.is_absolute() || !detail::endsWithCi(argv[0], ".exe")) {
    return {false, -1, "Windows runs only a native .exe CLI (got '" + argv[0] + "')"};
  }

  std::wstring cmdline;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      cmdline += L' ';
    }
    detail::appendQuotedArg(cmdline, detail::utf8ToWide(argv[i]));
  }
  if (cmdline.size() > 32766) {
    return {false, -1, "command line too long"};
  }

  SECURITY_ATTRIBUTES inheritable_sa{};
  inheritable_sa.nLength = sizeof(inheritable_sa);
  inheritable_sa.bInheritHandle = TRUE;
  inheritable_sa.lpSecurityDescriptor = nullptr;

  constexpr DWORD kPipeSize = 64 * 1024;
  HANDLE out_r_raw = nullptr, out_w_raw = nullptr;
  if (!CreatePipe(&out_r_raw, &out_w_raw, &inheritable_sa, kPipeSize)) {
    return {false, -1, "CreatePipe (stdout) failed"};
  }
  detail::ScopedHandle out_r(out_r_raw), out_w(out_w_raw);

  HANDLE in_r_raw = nullptr, in_w_raw = nullptr;
  if (!CreatePipe(&in_r_raw, &in_w_raw, &inheritable_sa, kPipeSize)) {
    return {false, -1, "CreatePipe (stdin) failed"};
  }
  detail::ScopedHandle in_r(in_r_raw), in_w(in_w_raw);

  // The parent's ends must not leak into another child spawned concurrently.
  SetHandleInformation(out_r.h, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(in_w.h, HANDLE_FLAG_INHERIT, 0);

  detail::ScopedHandle nul(CreateFileW(
      L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable_sa, OPEN_EXISTING, 0, nullptr));
  if (!nul) {
    return {false, -1, "could not open NUL for the child's stderr"};
  }

  // Explicit inheritance list: bInheritHandles=TRUE would otherwise hand the child every
  // inheritable handle this process holds, not just these three.
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in_r.h;
  si.StartupInfo.hStdOutput = out_w.h;
  si.StartupInfo.hStdError = nul.h;

  HANDLE handle_list[3] = {in_r.h, out_w.h, nul.h};
  SIZE_T attr_list_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_list_size);  // size query, expected to "fail"
  std::vector<char> attr_list_buf(attr_list_size);
  auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_list_buf.data());
  const bool attr_list_ok =
      InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_list_size) &&
      UpdateProcThreadAttribute(
          attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handle_list, sizeof(handle_list), nullptr, nullptr);
  if (!attr_list_ok) {
    return {false, -1, "could not build the child's handle-inheritance list"};
  }
  si.lpAttributeList = attr_list;

  detail::ScopedHandle job(CreateJobObjectW(nullptr, nullptr));
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
      job.reset();
    }
  }

  const std::wstring wapp = detail::utf8ToWide(argv[0]);
  std::wstring env_block = detail::buildEnvironmentBlock(extra_env);
  std::wstring wcwd;
  const wchar_t* cwd_ptr = nullptr;
  if (!working_dir.empty()) {
    wcwd = detail::utf8ToWide(working_dir);
    cwd_ptr = wcwd.c_str();
  }
  std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
  cmdline_buf.push_back(L'\0');  // CreateProcessW may write into this buffer

  PROCESS_INFORMATION pi{};
  const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
  const BOOL created = CreateProcessW(
      wapp.c_str(), cmdline_buf.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE, flags, env_block.data(), cwd_ptr,
      &si.StartupInfo, &pi);

  if (!created) {
    const DWORD err = GetLastError();
    DeleteProcThreadAttributeList(attr_list);
    return {false, -1, "CreateProcess failed (error " + std::to_string(err) + ")"};
  }
  detail::ScopedHandle process(pi.hProcess);

  // Close the child-side handles now to shrink the window in which a concurrent host spawn
  // could inherit them.
  in_r.reset();
  out_w.reset();
  nul.reset();
  DeleteProcThreadAttributeList(attr_list);

  if (job && !AssignProcessToJobObject(job.h, process.h)) {
    job.reset();  // cancel() below falls back to TerminateProcess
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  if (!stdin_data.empty()) {
    std::size_t off = 0;
    while (off < stdin_data.size()) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(stdin_data.size() - off, 1u << 20));
      DWORD written = 0;
      if (!WriteFile(in_w.h, stdin_data.data() + off, chunk, &written, nullptr) || written == 0) {
        break;
      }
      off += written;
    }
  }
  in_w.reset();

  bool killed = false;
  bool exited = false;
  int fallback_empty_waits = 0;  // only used without a job object
  char buf[4096];
  for (;;) {
    if (cancel.load() && !killed) {
      if (job) {
        TerminateJobObject(job.h, 1);
      } else {
        TerminateProcess(process.h, 1);
      }
      killed = true;
    }

    DWORD avail = 0;
    if (!PeekNamedPipe(out_r.h, nullptr, 0, nullptr, &avail, nullptr)) {
      break;  // ERROR_BROKEN_PIPE or worse: EOF
    }
    if (avail > 0) {
      const DWORD to_read = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
      DWORD n = 0;
      if (ReadFile(out_r.h, buf, to_read, &n, nullptr) && n > 0) {
        on_stdout(std::string(buf, n));
      }
      continue;
    }

    const DWORD wr = WaitForSingleObject(process.h, 50);
    if (wr == WAIT_OBJECT_0 && !exited) {
      exited = true;
      if (job) {
        // Kills descendants too: a grandchild's inherited copy of the stdout write handle
        // would otherwise keep the pipe open and the read loop waiting forever.
        TerminateJobObject(job.h, 0);
      }
    } else if (exited && !job) {
      // No job object to reap descendants (AssignProcessToJobObject failed): bound the wait
      // at 20 consecutive empty polls (~1 s) instead of blocking on a grandchild's handle.
      if (++fallback_empty_waits >= 20) {
        break;
      }
    }
  }

  WaitForSingleObject(process.h, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.h, &exit_code);
  // After a cancel, exit_code is the 1 passed to TerminateJobObject/TerminateProcess;
  // callers detect cancellation through their own flag, not this code.
  return {true, static_cast<int>(exit_code), ""};
}

}  // namespace assistant_agent

#else

namespace assistant_agent {
inline SubprocessResult runProcess(
    const std::vector<std::string>&, const std::string&, const std::function<void(const std::string&)>&,
    const std::atomic<bool>&, const std::string&, const std::vector<std::pair<std::string, std::string>>&) {
  return {false, -1, "subprocess is only supported on POSIX and Windows"};
}
}  // namespace assistant_agent

#endif
