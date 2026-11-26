// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace assistant_agent {

// Writes `contents` to a fresh private temp file named `name_prefix` + random
// suffix under the platform's temp directory, and sets `out_path` to it. A
// short write removes the file and returns false, leaving `out_path` untouched.
// For contents (MCP bearer token, instructions) that must not land on argv,
// which /proc/<pid>/cmdline exposes to every local user.
//
// POSIX: mkstemp, mode 0600. Windows has no mode bits: privacy comes from the
// per-user ACL on %TEMP%, and CREATE_NEW gives mkstemp's O_EXCL guarantee. No
// FILE_FLAG_DELETE_ON_CLOSE: the spawned CLI must still be able to open the file.
// Other platforms: always returns false.
[[nodiscard]] bool writePrivateTempFile(const char* name_prefix, const std::string& contents, std::string& out_path);

// Removes a file writePrivateTempFile created. Callers use it in destructors, so
// it reports no failure; an already-gone file is the desired outcome.
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
    // CREATE_NEW fails instead of reusing a name another process already claimed.
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
