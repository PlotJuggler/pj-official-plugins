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
// POSIX only. On other platforms it returns spawned=false so the caller can
// surface a clean "not supported here" instead of failing to build.
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

#else

namespace assistant_agent {
inline SubprocessResult runProcess(
    const std::vector<std::string>&, const std::string&, const std::function<void(const std::string&)>&,
    const std::atomic<bool>&, const std::string&, const std::vector<std::pair<std::string, std::string>>&) {
  return {false, -1, "subprocess is only supported on POSIX platforms"};
}
}  // namespace assistant_agent

#endif
