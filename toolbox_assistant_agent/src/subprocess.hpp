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

// Run argv[0] with argv, streaming its stdout to `on_stdout` in raw chunks
// (the caller splits into lines). argv is passed straight to execvp — there is
// NO shell, so nothing forwarded here can inject commands. `stdin_data` is
// written to the child's stdin then closed (keep it below the pipe buffer,
// ~64 KiB: it is written in full before stdout is drained); pass empty to give
// the child a closed stdin. Blocks until the child exits; if `cancel` becomes
// true the child is sent SIGTERM and reaped. stderr is discarded.
//
// A non-empty `working_dir` becomes the child's cwd (chdir after fork, before
// exec); the child exits 127 if it cannot enter it. Callers that hand the child
// to a tool which derives context from its cwd — the Claude CLI reads the
// working directory's CLAUDE.md and project state — pass a neutral directory so
// the child sees the caller's choice, not wherever the host app was launched.
//
// `extra_env` is applied with setenv() in the child, after fork and chdir but
// before exec — on top of the inherited environment, never replacing it. This
// is how CodexBackend hands the MCP bearer token to the CLI without it landing
// on the command line (argv is world-readable via /proc/<pid>/cmdline, exactly
// the reason ClaudeBackend already keeps its own token out of argv via a
// private --mcp-config file instead).
//
// POSIX and Windows; other platforms return spawned=false so the caller can
// surface a clean "not supported here" instead of failing to build. Windows
// differs from the POSIX contract above in a few load-bearing ways:
//   - a missing `working_dir` makes the call fail outright (spawned=false)
//     instead of the child exiting 127 -- CreateProcessW rejects a bad cwd
//     before the child ever starts, there is no fork() to survive past;
//   - `cancel` kills the whole process tree (a job object), not just the one
//     process SIGTERM would reach;
//   - argv[0] must be an absolute path to a native `.exe` -- handing
//     CreateProcess a `.bat`/`.cmd` routes it through cmd.exe, whose own
//     parsing is the "BatBadBut" injection class;
//   - once the child itself exits, anything IT started is killed too (via
//     the same job object) and this function then drains its stdout to EOF,
//     so a grandchild's inherited handle cannot keep the pipe open forever.
// stderr is discarded on every platform.
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
    // Child: stdin <- pipe, stdout -> pipe, stderr -> /dev/null, then exec.
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
      _exit(127);  // refusing to run in the wrong directory beats running there
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

  // Parent: hand the child its stdin up front (chat-sized, fits the pipe
  // buffer), close it so the child sees EOF, then drain stdout.
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
      continue;  // timeout — loop to re-check cancel
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

// A non-copyable owning wrapper around a Win32 HANDLE, closing it on
// destruction (both a null handle and INVALID_HANDLE_VALUE are treated as
// "nothing to close" -- CreateFileW hands back the latter on failure, every
// other API here hands back the former). Used for every handle runProcess
// creates below (pipes, the stderr sink, the job object, the child process)
// so the failure paths do not need a hand-copied CloseHandle chain: letting
// the wrapper go out of scope closes it.
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

  // Closes the current handle (if any) and takes ownership of `new_h`.
  void reset(HANDLE new_h = nullptr) {
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
    }
    h = new_h;
  }

  // Gives up ownership without closing, returning the raw handle.
  [[nodiscard]] HANDLE release() {
    HANDLE tmp = h;
    h = nullptr;
    return tmp;
  }

  explicit operator bool() const {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
  }
};

// Builds one CreateProcessW-ready argument, appended to `cmd`. Windows has no
// argv array -- the child re-splits a single command-line string itself (the
// same algorithm the Microsoft CRT startup code and CommandLineToArgvW use),
// so this exists to survive that round trip byte for byte. A bare backslash
// means nothing special; it only doubles right before a quote character
// (escaping it) or before the argument's own closing quote -- doubling it
// anywhere else would corrupt an ordinary path like `C:\Users\x\file`.
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

// Builds the double-NUL-terminated environment block CreateProcessW expects:
// every entry GetEnvironmentStringsW reports, with `extra_env` applied on
// top -- a name matching an existing entry case-insensitively replaces EVERY
// such entry (never leaving a shadowed duplicate for CreateProcessW to
// resolve unpredictably), a name matching nothing is appended -- then sorted
// case-insensitively by name, which is the shape a well-formed block has. A
// leading '=' entry (a per-drive hidden cwd, e.g. "=C:=C:\x") is kept
// verbatim: its NAME runs to the SECOND '=', not the first.
inline std::wstring buildEnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& extra_env) {
  struct Entry {
    std::wstring name;
    std::wstring line;  // "NAME=VALUE", or the raw "=C:=C:\x" form
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
    // Every entry whose name matches case-insensitively collapses into ONE
    // replacement -- if the inherited block somehow carried more than one
    // case-variant of the same name, the first match becomes the
    // replacement and any further match is erased rather than also
    // overwritten, which would otherwise leave duplicate "NAME=value" lines
    // behind for CreateProcessW to resolve unpredictably.
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
  block += L'\0';  // the block's own terminating NUL
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

  // argv[0] must be an absolute path to a native .exe -- see the Windows
  // paragraph in the contract comment above.
  const std::filesystem::path exe_path = utf8ToPath(argv[0]);
  if (!exe_path.is_absolute() || !detail::endsWithCi(argv[0], ".exe")) {
    return {false, -1, "Windows runs only a native .exe CLI (got '" + argv[0] + "')"};
  }

  // The single command-line string the child re-splits itself; every entry,
  // including argv[0], goes through the same quoting.
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

  // Only the child-side ends are meant to be inherited; the parent's own
  // ends must not leak into some OTHER child this process spawns
  // concurrently.
  SetHandleInformation(out_r.h, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(in_w.h, HANDLE_FLAG_INHERIT, 0);

  detail::ScopedHandle nul(CreateFileW(
      L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable_sa, OPEN_EXISTING, 0, nullptr));
  if (!nul) {
    return {false, -1, "could not open NUL for the child's stderr"};
  }

  // An explicit handle-inheritance list: without it, CreateProcess with
  // bInheritHandles=TRUE would hand the child every inheritable handle this
  // process happens to be holding at that moment, not just the three below.
  // The child's own copies are closed right after CreateProcess returns, to
  // shrink the window in which a concurrent host spawn could inherit them.
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in_r.h;
  si.StartupInfo.hStdOutput = out_w.h;
  si.StartupInfo.hStdError = nul.h;

  HANDLE handle_list[3] = {in_r.h, out_w.h, nul.h};
  SIZE_T attr_list_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_list_size);  // size query; expected to "fail"
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

  // Shrink the window in which a concurrent host spawn could inherit these:
  // close every child-side handle now that the child has its own copies.
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
  int fallback_empty_waits = 0;  // only used when job is absent; see below
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
      break;  // ERROR_BROKEN_PIPE (or worse): EOF
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
        // Kills every descendant too, so a grandchild's own inherited copy
        // of the stdout write handle cannot keep the pipe open after the
        // CLI itself is gone -- PeekNamedPipe/ReadFile above would otherwise
        // wait forever on output nothing will ever produce.
        TerminateJobObject(job.h, 0);
      }
    } else if (exited && !job) {
      // No job object to reap descendants with (AssignProcessToJobObject
      // failed): a grandchild's inherited handle could keep the pipe open
      // past the parent's own exit. Bounded fallback -- give up after 20
      // consecutive empty polls (~1 s) rather than block forever.
      if (++fallback_empty_waits >= 20) {
        break;
      }
    }
  }

  WaitForSingleObject(process.h, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.h, &exit_code);
  // out_r/process/job (and any still-open pipe handle above) close via their
  // ScopedHandle destructors below.
  // If `cancel` killed the child, exit_code is whatever TerminateJobObject/
  // TerminateProcess reported (1 above) -- callers detect cancellation
  // through their own `cancel_` flag, not through this exit code.
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
