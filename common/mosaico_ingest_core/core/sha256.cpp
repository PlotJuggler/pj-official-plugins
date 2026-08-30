// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "core/sha256.h"

#include <openssl/evp.h>

#include <exception>

namespace mosaico {

std::array<std::uint8_t, 32> sha256(std::string_view data) {
  std::array<std::uint8_t, 32> digest{};
  unsigned int digest_size = 0;
  const void* input = data.empty() ? static_cast<const void*>("") : static_cast<const void*>(data.data());
  const int result = EVP_Digest(input, data.size(), digest.data(), &digest_size, EVP_sha256(), nullptr);
  // Descriptor identities have no error representation; a crypto-provider
  // failure must stop the process instead of collapsing identities to bytes
  // that look like a valid digest.
  if (result != 1 || digest_size != digest.size()) {
    std::terminate();
  }
  return digest;
}

std::string sha256HexPrefix(std::string_view data, std::size_t bytes) {
  const auto digest = sha256(data);
  if (bytes > digest.size()) {
    bytes = digest.size();
  }
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (std::size_t i = 0; i < bytes; ++i) {
    out += kHex[digest[i] >> 4];
    out += kHex[digest[i] & 0xF];
  }
  return out;
}

}  // namespace mosaico
