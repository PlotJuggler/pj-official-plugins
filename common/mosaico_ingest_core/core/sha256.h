// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace mosaico {
/// Minimal FIPS 180-4 SHA-256 (vendored: the plugin declares no crypto
/// dependency; the descriptor identity is the only consumer).
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view data);
/// Lowercase hex of the FIRST `bytes` bytes of sha256(data). bytes <= 32.
[[nodiscard]] std::string sha256HexPrefix(std::string_view data, std::size_t bytes);
}  // namespace mosaico
