// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "descriptor_import/core/sha256.h"

#include <gtest/gtest.h>

#include <string>

namespace {
std::string hex(const std::array<std::uint8_t, 32>& d) {
  static const char* k = "0123456789abcdef";
  std::string out;
  for (auto b : d) {
    out += k[b >> 4];
    out += k[b & 0xF];
  }
  return out;
}
}  // namespace

TEST(Sha256, NistVectors) {
  EXPECT_EQ(hex(mosaico::sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(hex(mosaico::sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(
      hex(mosaico::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // One-million 'a' (streaming/block-boundary coverage).
  EXPECT_EQ(
      hex(mosaico::sha256(std::string(1'000'000, 'a'))),
      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, HexPrefix) {
  EXPECT_EQ(mosaico::sha256HexPrefix("abc", 16),
            "ba7816bf8f01cfea414140de5dae2223");  // first 16 bytes = 32 hex chars
  EXPECT_EQ(mosaico::sha256HexPrefix("abc", 32).size(), 64u);
}
