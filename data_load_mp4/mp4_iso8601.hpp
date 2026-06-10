// Parse a strict ISO-8601 UTC timestamp string into nanoseconds since the
// Unix epoch. Accepts the format produced by FFmpeg's `creation_time` tag:
//   YYYY-MM-DDTHH:MM:SS[.uuuuuu]Z
// Returns nullopt for any input that doesn't match (missing trailing Z, bad
// month, etc.).
//
// Pure: depends only on the C++ stdlib and Howard Hinnant's date lib.
#pragma once

#include <date/date.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace pj_mp4 {

[[nodiscard]] inline std::optional<int64_t> parseIso8601ToEpochNs(std::string_view iso) {
  if (iso.size() < 20 || iso.back() != 'Z') {
    return std::nullopt;
  }
  std::istringstream in{std::string(iso)};
  std::chrono::sys_time<std::chrono::microseconds> tp;
  // Qualified date::from_stream (not `>> date::parse`) so the call resolves
  // unambiguously to Howard Hinnant's date lib: under C++20 + libstdc++ 15 the
  // unqualified form is ambiguous with std::chrono::from_stream (ADL on
  // std::chrono::sys_time). Equivalent behavior on older toolchains.
  date::from_stream(in, "%FT%TZ", tp);
  if (in.fail()) {
    return std::nullopt;
  }
  return tp.time_since_epoch().count() * 1000LL;
}

}  // namespace pj_mp4
