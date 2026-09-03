// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace assistant_agent {

// Writes `contents` to a fresh 0600 temp file (mkstemp) whose name is
// `tmpl_prefix` + "XXXXXX" (e.g. "/tmp/pj_assistant_mcp_" ->
// "/tmp/pj_assistant_mcp_XXXXXX"), and sets `out_path` to the file actually
// created. A short write unlinks the file and returns false, leaving
// `out_path` untouched. Shared by McpLoopback::configFilePath (the
// --mcp-config file, bearer token inside) and
// CodexBackend::ensureInstructionsFile (the model_instructions_file) — both
// want a private file that never lands its sensitive contents on argv, which
// /proc/<pid>/cmdline exposes to every local user.
//
// POSIX only, same fallback shape as subprocess.hpp's runProcess.
[[nodiscard]] bool writePrivateTempFile(const char* tmpl_prefix, const std::string& contents, std::string& out_path);

}  // namespace assistant_agent

#if defined(__unix__) || defined(__APPLE__)

#include <unistd.h>  // mkstemp/write/close/unlink

namespace assistant_agent {

inline bool writePrivateTempFile(const char* tmpl_prefix, const std::string& contents, std::string& out_path) {
  std::string tmpl = std::string(tmpl_prefix) + "XXXXXX";
  const int fd = mkstemp(tmpl.data());
  if (fd < 0) {
    return false;
  }
  ssize_t off = 0;
  while (off < static_cast<ssize_t>(contents.size())) {
    const ssize_t w = write(fd, contents.data() + off, contents.size() - static_cast<std::size_t>(off));
    if (w <= 0) {
      break;
    }
    off += w;
  }
  close(fd);
  if (off != static_cast<ssize_t>(contents.size())) {
    unlink(tmpl.c_str());
    return false;
  }
  out_path = std::move(tmpl);
  return true;
}

}  // namespace assistant_agent

#else

namespace assistant_agent {
inline bool writePrivateTempFile(const char*, const std::string&, std::string&) {
  return false;
}
}  // namespace assistant_agent

#endif
