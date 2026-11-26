// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>

namespace assistant_agent {

// UTF-8 <-> std::filesystem::path conversions, plus environment and home-directory lookups
// that answer in UTF-8. Every narrow std::string in this plugin is UTF-8, which POSIX
// already speaks. On MSVC, std::filesystem::path::string() and std::getenv() use the ANSI
// codepage and would mangle non-ASCII characters in a subprocess argv or a path round trip;
// these four functions are the one place that narrowing happens.

// POSIX: `p.string()`. Windows: `p.u8string()` narrowed, not the ANSI `p.string()`.
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& p);

// The inverse. Windows reads `s` as UTF-8 (via `std::u8string`), not the ANSI codepage.
[[nodiscard]] std::filesystem::path utf8ToPath(const std::string& s);

// POSIX: `PJ::sdk::getEnv(name)`. Windows: `GetEnvironmentVariableW` narrowed to UTF-8.
// nullopt when unset or empty, like PJ::sdk::getEnv.
[[nodiscard]] std::optional<std::string> getEnvUtf8(const char* name);

// `HOME` first (tests set it even on Windows via ScopedEnv), then on Windows `USERPROFILE`.
// nullopt when neither resolves.
[[nodiscard]] std::optional<std::string> userHomeDir();

}  // namespace assistant_agent

namespace assistant_agent::detail {

// ASCII-only case-insensitive suffix match, for Windows extensions (`.exe`/`.cmd`/`.ps1`).
// Platform-neutral; only the Windows code paths use it.
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

// UTF-8 <-> UTF-16 conversions for the Windows branches (subprocess.hpp, cli_locator.hpp).
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
  // Null buffer: returns the needed length INCLUDING the terminating NUL, or 0 when unset.
  const DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (needed == 0) {
    return std::nullopt;
  }
  std::wstring buf(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = GetEnvironmentVariableW(wname.c_str(), buf.data(), needed);
  if (written == 0) {
    return std::nullopt;
  }
  buf.resize(written);  // drop the NUL counted in `needed`
  std::string value = detail::wideToUtf8(buf);
  if (value.empty()) {
    return std::nullopt;  // like PJ::sdk::getEnv: empty means absent
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
