// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>

namespace assistant_agent {

// UTF-8 <-> std::filesystem::path conversions, plus environment-variable and
// home-directory lookups that answer in UTF-8. Every narrow std::string in
// this plugin is UTF-8; on POSIX that is already what the filesystem and
// getenv() speak, so these are the identity. On MSVC, both
// std::filesystem::path::string() and std::getenv() go through the ANSI
// codepage instead -- feeding either straight into a subprocess's argv, or
// round-tripping a non-ASCII path through a std::string, would silently
// mangle any character outside it. These four functions are the one place
// that narrowing happens, so every other Windows branch in this plugin can
// stay UTF-8 throughout instead of re-deriving the conversion.

// `p.string()` on POSIX (already UTF-8); on Windows, `p.u8string()` narrowed
// to `std::string` -- UTF-8, not the ANSI codepage `p.string()` would give.
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& p);

// The inverse: `path(s)` on POSIX; on Windows `s` is first read as UTF-8
// (via `std::u8string`) rather than the ANSI codepage `path(s)` would assume.
[[nodiscard]] std::filesystem::path utf8ToPath(const std::string& s);

// `PJ::sdk::getEnv(name)` on POSIX (already UTF-8); on Windows,
// `GetEnvironmentVariableW` narrowed to UTF-8 -- `std::getenv()` there reads
// the ANSI codepage and would mangle anything outside it. nullopt when the
// variable is unset or empty, matching PJ::sdk::getEnv's own contract.
[[nodiscard]] std::optional<std::string> getEnvUtf8(const char* name);

// `HOME` first -- the existing test fixtures (ScopedEnv) set it even on
// Windows, so tests do not depend on the real signed-in profile -- then, on
// Windows only, `USERPROFILE` when `HOME` is absent. nullopt when neither
// resolves.
[[nodiscard]] std::optional<std::string> userHomeDir();

}  // namespace assistant_agent

namespace assistant_agent::detail {

// Case-insensitive suffix match, ASCII only -- Windows filenames and
// extensions are case-insensitive throughout this plugin (`.exe`/`.cmd`/
// `.ps1`, PATH-entry stripping), never the codepage-sensitive kind the four
// functions above guard against, since every suffix compared here is ASCII.
// Plain std::string comparison, no platform API needed, so it is available
// on every platform (subprocess.hpp's Windows runProcess and cli_locator.hpp's
// Windows locateCli both use it; nothing off Windows currently does).
inline bool endsWithCi(const std::string& s, const std::string& suffix) {
  if (s.size() < suffix.size()) {
    return false;
  }
  return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  });
}

}  // namespace assistant_agent::detail

#if defined(__unix__) || defined(__APPLE__)

#include <pj_base/sdk/platform.hpp>  // PJ::sdk::getEnv

namespace assistant_agent {

inline std::string pathToUtf8(const std::filesystem::path& p) {
  return p.string();
}

inline std::filesystem::path utf8ToPath(const std::string& s) {
  return std::filesystem::path(s);
}

inline std::optional<std::string> getEnvUtf8(const char* name) {
  return PJ::sdk::getEnv(name);
}

inline std::optional<std::string> userHomeDir() {
  return PJ::sdk::getEnv("HOME");
}

}  // namespace assistant_agent

#elif defined(_WIN32)

#include <windows.h>

namespace assistant_agent {

// Windows-only: the UTF-8 <-> UTF-16 conversions every Windows branch in this
// plugin needs (subprocess.hpp's argv/env/cwd, cli_locator.hpp's PATH
// search). Kept here rather than duplicated per file.
namespace detail {

inline std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

inline std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

}  // namespace detail

inline std::string pathToUtf8(const std::filesystem::path& p) {
  auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

inline std::filesystem::path utf8ToPath(const std::string& s) {
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

inline std::optional<std::string> getEnvUtf8(const char* name) {
  const std::wstring wname = detail::utf8ToWide(name);
  // Called first with a null buffer purely to size it: the return value is
  // the needed length INCLUDING the terminating NUL, or 0 when the variable
  // is not set at all (GetLastError() == ERROR_ENVVAR_NOT_FOUND).
  const DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (needed == 0) {
    return std::nullopt;
  }
  std::wstring buf(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = GetEnvironmentVariableW(wname.c_str(), buf.data(), needed);
  if (written == 0) {
    return std::nullopt;
  }
  buf.resize(written);  // exclude the trailing NUL GetEnvironmentVariableW counted in `needed`
  std::string value = detail::wideToUtf8(buf);
  if (value.empty()) {
    return std::nullopt;  // matches PJ::sdk::getEnv: empty collapses to absent
  }
  return value;
}

inline std::optional<std::string> userHomeDir() {
  if (auto home = getEnvUtf8("HOME")) {
    return home;
  }
  return getEnvUtf8("USERPROFILE");
}

}  // namespace assistant_agent

#else

namespace assistant_agent {

inline std::string pathToUtf8(const std::filesystem::path&) {
  return {};
}
inline std::filesystem::path utf8ToPath(const std::string&) {
  return {};
}
inline std::optional<std::string> getEnvUtf8(const char*) {
  return std::nullopt;
}
inline std::optional<std::string> userHomeDir() {
  return std::nullopt;
}

}  // namespace assistant_agent

#endif
