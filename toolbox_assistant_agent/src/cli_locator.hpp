// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace assistant_agent {

// Where a configured CLI name/path actually resolved, and what was tried.
struct CliLocation {
  std::string path;                   // the executable to use as argv[0] ('' if not found)
  std::string bin_dir;                // its containing directory, for the child's PATH
  std::vector<std::string> searched;  // directories actually probed, in order, for the error message
  // Windows only: a `.cmd`/`.ps1` launcher (the shape npm installs a CLI as)
  // found where no native `.exe` was. Never executed -- CreateProcess only
  // runs a native .exe (see subprocess.hpp); this is here purely so
  // cannotFindCliMessage can name it and point at the native installer
  // instead of leaving the user to guess why a CLI that clearly "works from
  // a terminal" cannot be spawned. LAST member so nothing before it shifts.
  std::string npm_shim;
};

// Resolve `configured` (the Settings "CLI path" value, or a bare name like "claude"/"codex") to
// an executable. POSIX and Windows only — see the blocks below; other platforms return
// `configured` as-is, with nothing searched, so the build stays clean without pretending to
// search anywhere.
[[nodiscard]] CliLocation locateCli(const std::string& configured);

// The PATH override to hand to runProcess's `extra_env` so a child whose shebang is
// `#!/usr/bin/env node` (npm-installed `codex` under nvm, notably) finds its own interpreter even
// when `loc.bin_dir` never made it into this process's own PATH. Empty when `loc.bin_dir` is
// empty — nothing to prepend, nothing to override.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> childEnvFor(const CliLocation& loc);

}  // namespace assistant_agent

#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>  // access
#endif

#include "platform_util.hpp"  // pathToUtf8, utf8ToPath, userHomeDir, getEnvUtf8, detail::endsWithCi

namespace assistant_agent {

// Shared between the POSIX and Windows implementations below: everything
// that does not depend on which platform's search order calls it.
namespace detail {

#if defined(__unix__) || defined(__APPLE__)

inline bool isExecutableFile(const std::string& path) {
  return !path.empty() && access(path.c_str(), X_OK) == 0;
}

#elif defined(_WIN32)

// A CLI here means a native .exe — see subprocess.hpp's Windows contract.
// endsWithCi is platform_util.hpp's (assistant_agent::detail), unqualified
// lookup finds it since this reopens the same namespace.
inline bool isExecutableFile(const std::string& path) {
  if (path.empty() || !endsWithCi(path, ".exe")) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(utf8ToPath(path), ec) && !ec;
}

#endif

// Expands a leading "~/" (and, on Windows, "~\") using the home directory;
// every other path (relative, or already absolute) is returned unchanged.
// No home directory resolved leaves the tilde in place — the existence
// check just fails on it.
inline std::string expandTilde(const std::string& path) {
  const bool tilde_slash = path.size() >= 2 && path[0] == '~' && path[1] == '/';
#if defined(_WIN32)
  const bool tilde_backslash = path.size() >= 2 && path[0] == '~' && path[1] == '\\';
#else
  const bool tilde_backslash = false;
#endif
  if (tilde_slash || tilde_backslash) {
    if (const auto home = userHomeDir()) {
      return *home + path.substr(1);
    }
  }
  return path;
}

inline std::vector<std::string> splitPath(const std::string& path_env, char sep) {
  std::vector<std::string> dirs;
  std::string cur;
  for (const char c : path_env) {
    if (c == sep) {
      dirs.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  dirs.push_back(cur);
  return dirs;
}

// A directory joins `searched` at most once, in the order it was first probed — the same
// directory reachable through both PATH and a fallback (e.g. PATH already contains
// ~/.local/bin) should not read like two separate misses in the error message.
inline void addSearched(std::vector<std::string>& searched, const std::string& dir) {
  if (!dir.empty() && std::find(searched.begin(), searched.end(), dir) == searched.end()) {
    searched.push_back(dir);
  }
}

// Try `dir/name` (`dir\name` on Windows — built through std::filesystem::path
// so the separator is always the native one); on success append it (as
// bin_dir/path pair via the caller) and report true.
inline bool tryDir(
    const std::string& dir, const std::string& name, std::vector<std::string>& searched, std::string& found_path,
    std::string& found_bin_dir) {
  addSearched(searched, dir);
  const std::string candidate = pathToUtf8(utf8ToPath(dir) / utf8ToPath(name));
  if (isExecutableFile(candidate)) {
    found_path = candidate;
    found_bin_dir = dir;
    return true;
  }
  return false;
}

}  // namespace detail

}  // namespace assistant_agent

#endif  // POSIX || Windows

#if defined(__unix__) || defined(__APPLE__)

#include <fstream>
#include <tuple>

namespace assistant_agent {

namespace detail {

// Parses "vMAJOR.MINOR.PATCH" or "MAJOR.MINOR.PATCH" into three ints; false (and an all-zero
// triple) if it doesn't look like a semver at all — callers use that to skip non-numeric alias
// contents ("lts/*", "node") and non-version directory entries alike.
inline bool parseSemver(const std::string& raw, int& major, int& minor, int& patch) {
  std::string s = raw;
  if (!s.empty() && s[0] == 'v') {
    s = s.substr(1);
  }
  major = minor = patch = 0;
  int* slots[3] = {&major, &minor, &patch};
  std::size_t slot = 0;
  std::size_t i = 0;
  while (slot < 3u) {
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
    int value = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      value = value * 10 + (s[i] - '0');
      ++i;
    }
    *slots[slot] = value;
    ++slot;
    if (slot < 3u) {
      if (i >= s.size() || s[i] != '.') {
        return false;
      }
      ++i;
    }
  }
  return true;
}

// Resolves the nvm node bin/ directory to search, or "" if nvm is not installed at all: the
// alias's numeric version when useful, else the highest installed version by numeric semver
// comparison (never lexicographic — "v9.0.0" must lose to "v22.0.0").
inline std::string nvmBinDir(const std::string& home) {
  const std::string nvm_dir = home + "/.nvm";
  const std::string versions_dir = nvm_dir + "/versions/node";
  std::error_code ec;
  if (!std::filesystem::exists(versions_dir, ec)) {
    return {};
  }

  const std::string alias_path = nvm_dir + "/alias/default";
  std::ifstream alias_file(alias_path);
  if (alias_file) {
    std::string alias_contents;
    std::getline(alias_file, alias_contents);
    int major = 0, minor = 0, patch = 0;
    if (parseSemver(alias_contents, major, minor, patch)) {
      const std::string dir = versions_dir + "/v" + std::to_string(major) + "." + std::to_string(minor) + "." +
                              std::to_string(patch) + "/bin";
      if (std::filesystem::exists(dir, ec)) {
        return dir;
      }
    }
  }

  std::string best_dir;
  int best_major = -1, best_minor = -1, best_patch = -1;
  for (const auto& entry : std::filesystem::directory_iterator(versions_dir, ec)) {
    if (!entry.is_directory()) {
      continue;
    }
    int major = 0, minor = 0, patch = 0;
    if (!parseSemver(entry.path().filename().string(), major, minor, patch)) {
      continue;
    }
    const bool better = std::tie(major, minor, patch) > std::tie(best_major, best_minor, best_patch);
    if (better) {
      best_major = major;
      best_minor = minor;
      best_patch = patch;
      best_dir = entry.path().string() + "/bin";
    }
  }
  return best_dir;
}

}  // namespace detail

inline CliLocation locateCli(const std::string& configured) {
  CliLocation loc;
  if (configured.empty()) {
    return loc;
  }

  if (configured.find('/') != std::string::npos) {
    const std::string expanded = detail::expandTilde(configured);
    detail::addSearched(loc.searched, expanded);
    if (detail::isExecutableFile(expanded)) {
      loc.path = expanded;
      loc.bin_dir = std::filesystem::path(expanded).parent_path().string();
    }
    return loc;
  }

  const std::string path_env = getEnvUtf8("PATH").value_or("");
  for (const std::string& dir : detail::splitPath(path_env, ':')) {
    if (dir.empty()) {
      continue;  // an empty PATH entry means "current directory" in the shell, never intended here
    }
    if (detail::tryDir(dir, configured, loc.searched, loc.path, loc.bin_dir)) {
      return loc;
    }
  }

  const auto home = userHomeDir();
  if (home.has_value() && !home->empty()) {
    if (detail::tryDir(*home + "/.local/bin", configured, loc.searched, loc.path, loc.bin_dir)) {
      return loc;
    }
    // nvmBinDir() already picked the one version directory worth trying (the alias, or else the
    // highest installed version); nothing else under ~/.nvm needs probing.
    const std::string nvm_bin = detail::nvmBinDir(*home);
    if (!nvm_bin.empty() && detail::tryDir(nvm_bin, configured, loc.searched, loc.path, loc.bin_dir)) {
      return loc;
    }
    if (detail::tryDir(*home + "/.npm-global/bin", configured, loc.searched, loc.path, loc.bin_dir)) {
      return loc;
    }
    if (detail::tryDir(*home + "/.volta/bin", configured, loc.searched, loc.path, loc.bin_dir)) {
      return loc;
    }
  }

  for (const char* dir : {"/usr/local/bin", "/opt/homebrew/bin", "/home/linuxbrew/.linuxbrew/bin"}) {
    if (detail::tryDir(dir, configured, loc.searched, loc.path, loc.bin_dir)) {
      return loc;
    }
  }

  return loc;
}

inline std::vector<std::pair<std::string, std::string>> childEnvFor(const CliLocation& loc) {
  if (loc.bin_dir.empty()) {
    return {};
  }
  return {{"PATH", loc.bin_dir + ":" + getEnvUtf8("PATH").value_or("")}};
}

}  // namespace assistant_agent

#elif defined(_WIN32)

namespace assistant_agent {

inline CliLocation locateCli(const std::string& configured) {
  CliLocation loc;
  if (configured.empty()) {
    return loc;
  }

  if (configured.find('/') != std::string::npos || configured.find('\\') != std::string::npos) {
    // An explicit path (relative or absolute): resolve it directly, never
    // against PATH or any fallback directory.
    const std::string expanded = detail::expandTilde(configured);
    detail::addSearched(loc.searched, expanded);

    if (detail::isExecutableFile(expanded)) {
      loc.path = expanded;
      loc.bin_dir = pathToUtf8(utf8ToPath(expanded).parent_path());
      return loc;
    }

    const std::filesystem::path expanded_path = utf8ToPath(expanded);
    if (expanded_path.extension().empty()) {
      const std::string with_exe = expanded + ".exe";
      if (detail::isExecutableFile(with_exe)) {
        loc.path = with_exe;
        loc.bin_dir = pathToUtf8(utf8ToPath(with_exe).parent_path());
        return loc;
      }
    }

    // No native .exe anywhere: settle for identifying an npm launcher so
    // cannotFindCliMessage (cli_probe.hpp) can name it instead of leaving
    // the user to guess. It is never executed (see CliLocation::npm_shim).
    std::error_code ec;
    const bool is_shim_ext = detail::endsWithCi(expanded, ".cmd") || detail::endsWithCi(expanded, ".bat") ||
                             detail::endsWithCi(expanded, ".ps1");
    if (is_shim_ext && std::filesystem::is_regular_file(expanded_path, ec)) {
      loc.npm_shim = expanded;
      return loc;
    }
    const std::string cmd_candidate = expanded + ".cmd";
    if (std::filesystem::is_regular_file(utf8ToPath(cmd_candidate), ec)) {
      loc.npm_shim = cmd_candidate;
    }
    return loc;
  }

  // A bare name: `exe` is what tryDir probes for (always ending in .exe —
  // Windows runs no other kind of CLI, see subprocess.hpp); `stem` is what
  // an npm shim would be named instead, e.g. "codex.cmd" beside a missing
  // "codex.exe".
  const bool already_exe = detail::endsWithCi(configured, ".exe");
  const std::string exe = already_exe ? configured : configured + ".exe";
  const std::string stem = already_exe ? configured.substr(0, configured.size() - 4) : configured;

  const auto probeShimIn = [&](const std::string& dir) {
    if (!loc.npm_shim.empty()) {
      return;
    }
    std::error_code ec;
    const std::string cmd_candidate = pathToUtf8(utf8ToPath(dir) / utf8ToPath(stem + ".cmd"));
    if (std::filesystem::is_regular_file(utf8ToPath(cmd_candidate), ec)) {
      loc.npm_shim = cmd_candidate;
      return;
    }
    const std::string ps1_candidate = pathToUtf8(utf8ToPath(dir) / utf8ToPath(stem + ".ps1"));
    if (std::filesystem::is_regular_file(utf8ToPath(ps1_candidate), ec)) {
      loc.npm_shim = ps1_candidate;
    }
  };

  const std::string path_env = getEnvUtf8("PATH").value_or("");
  for (std::string dir : detail::splitPath(path_env, ';')) {
    // PATH entries containing a space are commonly quoted end to end
    // ("C:\Program Files\x"); strip that before using the directory.
    if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
      dir = dir.substr(1, dir.size() - 2);
    }
    if (dir.empty()) {
      continue;
    }
    if (detail::tryDir(dir, exe, loc.searched, loc.path, loc.bin_dir)) {
      return loc;
    }
    probeShimIn(dir);
  }

  if (const auto home = userHomeDir(); home.has_value() && !home->empty()) {
    // The native Claude CLI installer's default location.
    const std::string local_bin = pathToUtf8(utf8ToPath(*home) / utf8ToPath(".local") / utf8ToPath("bin"));
    if (detail::tryDir(local_bin, exe, loc.searched, loc.path, loc.bin_dir)) {
      return loc;
    }
  }
  if (const auto local_appdata = getEnvUtf8("LOCALAPPDATA")) {
    const std::string winget_links =
        pathToUtf8(utf8ToPath(*local_appdata) / utf8ToPath("Microsoft") / utf8ToPath("WinGet") / utf8ToPath("Links"));
    if (detail::tryDir(winget_links, exe, loc.searched, loc.path, loc.bin_dir)) {
      return loc;
    }
  }

  // Last resort: an npm shim under %APPDATA%\npm, checked but never added
  // to `searched` — this is not a directory the plugin otherwise searches,
  // only a place to look for something to NAME in the "install the native
  // build instead" message.
  if (const auto appdata = getEnvUtf8("APPDATA")) {
    probeShimIn(pathToUtf8(utf8ToPath(*appdata) / utf8ToPath("npm")));
  }

  return loc;
}

inline std::vector<std::pair<std::string, std::string>> childEnvFor(const CliLocation& loc) {
  if (loc.bin_dir.empty()) {
    return {};
  }
  // runProcess's Windows environment-block builder (subprocess.hpp) replaces
  // an existing "Path" entry case-insensitively, so the literal casing of
  // the key here does not matter.
  return {{"PATH", loc.bin_dir + ";" + getEnvUtf8("PATH").value_or("")}};
}

}  // namespace assistant_agent

#else

namespace assistant_agent {

inline CliLocation locateCli(const std::string& configured) {
  return {configured, "", {}, ""};
}

inline std::vector<std::pair<std::string, std::string>> childEnvFor(const CliLocation&) {
  return {};
}

}  // namespace assistant_agent

#endif
