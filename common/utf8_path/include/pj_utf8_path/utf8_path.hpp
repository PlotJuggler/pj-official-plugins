// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

/// @file utf8_path.hpp
/// @brief UTF-8 <-> std::filesystem::path conversion for the paths the host hands to plugins.
///
/// The host passes file paths as UTF-8 `std::string`. On Windows a narrow string given to
/// `std::filesystem::path`, `std::ifstream`, `std::ofstream`, `fopen` or `CreateFileA` is read in
/// the ANSI code page, so a path with Cyrillic, CJK or accented characters opens the wrong file or
/// none. Likewise `path.string()` yields ANSI bytes (or throws on MSVC for characters the code page
/// cannot represent), which is wrong for anything sent to the host, the UI, Arrow or FFmpeg.
/// Build every path that comes from the host with pathFromUtf8() and turn every path that goes back
/// into a `std::string` with pathToUtf8(). On POSIX both are byte copies.
///
/// Header-only: consumers link the INTERFACE target `pj_utf8_path` and
/// `#include <pj_utf8_path/utf8_path.hpp>`.

#include <filesystem>
#include <string>
#include <string_view>

namespace PJ::utf8_path {

/// The bytes of a UTF-8 text as a `std::string`. Write a non-ASCII literal as `u8"...\u043f..."`: MSVC reads a
/// narrow literal, or a raw non-ASCII character in a `u8` literal, in the ANSI code page unless built with /utf-8.
inline std::string utf8String(std::u8string_view text) {
  return std::string(text.begin(), text.end());
}

inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string pathToUtf8(const std::filesystem::path& path) {
  return utf8String(path.u8string());
}

}  // namespace PJ::utf8_path
