// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace assistant_agent {

// Writes `contents` to a fresh 0600 temp file (mkstemp) named `name_prefix` +
// "XXXXXX" under the platform's own temp directory (e.g. name_prefix
// "pj_assistant_mcp_" -> POSIX "/tmp/pj_assistant_mcp_XXXXXX"), and sets
// `out_path` to the file actually created. A short write unlinks the file and
// returns false, leaving `out_path` untouched. Shared by
// McpLoopback::configFilePath (the --mcp-config file, bearer token inside)
// and CodexBackend::ensureInstructionsFile (the model_instructions_file) —
// both want a private file that never lands its sensitive contents on argv,
// which /proc/<pid>/cmdline exposes to every local user.
//
// `name_prefix` is a bare file-name prefix such as "pj_assistant_mcp_"; each
// platform chooses the directory.
//
// POSIX and Windows, same fallback shape as subprocess.hpp's runProcess for
// other platforms. Windows has no mode bits: the stand-in for 0600 is the
// per-user ACL that already guards everything under %TEMP% (it lives inside
// the signed-in user's own profile), and CREATE_NEW is what gives the same
// "nobody else could have raced this name into existence" guarantee mkstemp
// gets from O_EXCL. There is no FILE_FLAG_DELETE_ON_CLOSE, unlike a POSIX
// unlink-after-open pattern would use: the file has to still be openable by
// the CLI process this plugin spawns afterwards.
[[nodiscard]] bool writePrivateTempFile(const char* name_prefix, const std::string& contents, std::string& out_path);

// Removes a file writePrivateTempFile created. Callers hold these paths in a
// member and drop them in a destructor, so this cannot report failure: an
// already-gone file is the outcome they wanted. No-op off POSIX and Windows,
// where writePrivateTempFile never created one.
void removePrivateTempFile(const std::string& path);

}  // namespace assistant_agent

#if defined(__unix__) || defined(__APPLE__)

#include <unistd.h>  // mkstemp/write/close/unlink

namespace assistant_agent {

inline bool writePrivateTempFile(const char* name_prefix, const std::string& contents, std::string& out_path) {
  std::string tmpl = "/tmp/" + std::string(name_prefix) + "XXXXXX";
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

inline void removePrivateTempFile(const std::string& path) {
  if (!path.empty()) {
    unlink(path.c_str());
  }
}

}  // namespace assistant_agent

#elif defined(_WIN32)

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <random>
#include <system_error>

#include "platform_util.hpp"  // pathToUtf8, utf8ToPath

namespace assistant_agent {

inline bool writePrivateTempFile(const char* name_prefix, const std::string& contents, std::string& out_path) {
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return false;
  }

  std::random_device rd;
  for (int attempt = 0; attempt < 16; ++attempt) {
    char suffix[17];
    std::snprintf(suffix, sizeof(suffix), "%08x%08x", rd(), rd());
    const std::filesystem::path candidate = dir / (std::string(name_prefix) + suffix);
    // CREATE_NEW is the Windows equivalent of mkstemp's O_EXCL guarantee:
    // this call fails instead of silently reusing a name another process (or
    // another attempt in this same loop, astronomically unlikely as that is)
    // already claimed.
    const HANDLE h =
        CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_EXISTS) {
        continue;
      }
      return false;
    }
    std::size_t written_total = 0;
    while (written_total < contents.size()) {
      DWORD written = 0;
      const DWORD chunk = static_cast<DWORD>(contents.size() - written_total);
      if (!WriteFile(h, contents.data() + written_total, chunk, &written, nullptr) || written == 0) {
        break;
      }
      written_total += written;
    }
    CloseHandle(h);
    if (written_total != contents.size()) {
      DeleteFileW(candidate.c_str());
      return false;
    }
    out_path = pathToUtf8(candidate);
    return true;
  }
  return false;
}

inline void removePrivateTempFile(const std::string& path) {
  if (!path.empty()) {
    DeleteFileW(utf8ToPath(path).c_str());
  }
}

}  // namespace assistant_agent

#else

namespace assistant_agent {
inline bool writePrivateTempFile(const char*, const std::string&, std::string&) {
  return false;
}
inline void removePrivateTempFile(const std::string&) {}
}  // namespace assistant_agent

#endif
