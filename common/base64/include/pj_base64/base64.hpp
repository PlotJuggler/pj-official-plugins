#pragma once

/// @file base64.hpp
/// @brief Minimal, dependency-free base64 decoder shared across plugins.
///
/// Header-only so consumers just link the INTERFACE target `pj_base64` and
/// `#include <pj_base64/base64.hpp>`. Binary-safe: the decoded output may
/// contain embedded NUL bytes (e.g. a protobuf FileDescriptorSet), so it is
/// returned as a std::string used as a byte buffer.

#include <cstdint>
#include <string>
#include <string_view>

namespace PJ::base64 {

/// Decode a standard base64 string. Invalid quartets are skipped (lenient,
/// matching the historical behaviour this consolidates). `=` padding is honored.
inline std::string decode(std::string_view input) {
  static const int kDecodeTable[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55,
      56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
      13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32,
      33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
  };

  std::string output;
  output.reserve((input.size() / 4) * 3);

  for (size_t i = 0; i < input.size(); i += 4) {
    int n0 = kDecodeTable[static_cast<uint8_t>(input[i])];
    int n1 = (i + 1 < input.size()) ? kDecodeTable[static_cast<uint8_t>(input[i + 1])] : 0;
    int n2 = (i + 2 < input.size() && input[i + 2] != '=') ? kDecodeTable[static_cast<uint8_t>(input[i + 2])] : 0;
    int n3 = (i + 3 < input.size() && input[i + 3] != '=') ? kDecodeTable[static_cast<uint8_t>(input[i + 3])] : 0;

    if (n0 < 0 || n1 < 0) {
      continue;
    }

    output.push_back(static_cast<char>((n0 << 2) | (n1 >> 4)));
    if (i + 2 < input.size() && input[i + 2] != '=') {
      output.push_back(static_cast<char>(((n1 & 0x0F) << 4) | (n2 >> 2)));
    }
    if (i + 3 < input.size() && input[i + 3] != '=') {
      output.push_back(static_cast<char>(((n2 & 0x03) << 6) | n3));
    }
  }
  return output;
}

}  // namespace PJ::base64
