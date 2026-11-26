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
  std::vector<std::string> searched;  // directories probed, in order, for the error message
  // Windows only: a `.cmd`/`.ps1` launcher (how npm installs a CLI) found where no native
  // `.exe` was. Never executed (CreateProcess runs only a native .exe, see subprocess.hpp);
  // cannotFindCliMessage names it so the user is pointed at the native installer. LAST member
  // so nothing before it shifts.
  std::string npm_shim;
};

// Resolves `configured` (the Settings "CLI path", or a bare name like "claude"/"codex") to an
// executable. POSIX and Windows only; other platforms return `configured` as-is, nothing searched.
[[nodiscard]] CliLocation locateCli(const std::string& configured);

// The PATH override for runProcess's `extra_env`, so a child whose shebang is
// `#!/usr/bin/env node` (npm-installed `codex` under nvm) finds its own interpreter even when
// `loc.bin_dir` is not in this process's PATH. Empty when `loc.bin_dir` is empty.
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

// Shared by the POSIX and Windows implementations below.
namespace detail {

#if defined(__unix__) || defined(__APPLE__)

inline bool isExecutableFile(const std::string& path) {
  return !path.empty() && access(path.c_str(), X_OK) == 0;
}

#elif defined(_WIN32)

// A CLI here means a native .exe (subprocess.hpp's Windows contract).
inline bool isExecutableFile(const std::string& path) {
  if (path.empty() || !endsWithCi(path, ".exe")) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(utf8ToPath(path), ec) && !ec;
}

#endif

// Expands a leading "~/" (on Windows also "~\"); other paths are unchanged. Without a home
// directory the tilde stays and the existence check fails on it.
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

// A directory joins `searched` at most once, so one reachable through both PATH and a
// fallback does not read as two misses in the error message.
inline void addSearched(std::vector<std::string>& searched, const std::string& dir) {
  if (!dir.empty() && std::find(searched.begin(), searched.end(), dir) == searched.end()) {
    searched.push_back(dir);
  }
}

// Tries `dir/name`, built through std::filesystem::path so the separator is native; on
// success fills `found_path`/`found_bin_dir` and returns true.
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

// Parses "vMAJOR.MINOR.PATCH" or "MAJOR.MINOR.PATCH"; false (all-zero triple) for anything
// else, which skips non-numeric nvm aliases ("lts/*", "node") and non-version directories.
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

// The nvm node bin/ directory to search, or "" if nvm is not installed: the default alias's
// version if numeric, else the highest installed by numeric semver comparison (never
// lexicographic: "v9.0.0" must lose to "v22.0.0").
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
      continue;  // an empty PATH entry means the current directory in a shell; never intended here
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
    // An explicit path is resolved directly, never against PATH or fallback directories.
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

    // No native .exe: identify an npm launcher so cannotFindCliMessage can name it
    // (never executed, see CliLocation::npm_shim).
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

  // A bare name: `exe` is what tryDir probes (always .exe, see subprocess.hpp); `stem` names
  // an npm shim instead, e.g. "codex.cmd" beside a missing "codex.exe".
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
    // PATH entries with a space are commonly quoted end to end ("C:\Program Files\x").
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
    // The native Claude CLI installer's location.
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

  // Last resort: an npm shim under %APPDATA%\npm, only to NAME it in the "install the native
  // build" message, so it is not added to `searched`.
  if (const auto appdata = getEnvUtf8("APPDATA")) {
    probeShimIn(pathToUtf8(utf8ToPath(*appdata) / utf8ToPath("npm")));
  }

  return loc;
}

inline std::vector<std::pair<std::string, std::string>> childEnvFor(const CliLocation& loc) {
  if (loc.bin_dir.empty()) {
    return {};
  }
  // runProcess's environment-block builder replaces an existing "Path" case-insensitively.
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
