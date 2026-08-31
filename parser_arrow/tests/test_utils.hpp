#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace pj::parser_arrow::test {

/// Read a complete binary fixture or throw when the path cannot be read.
[[nodiscard]] inline std::vector<uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    throw std::runtime_error("Failed to open test fixture: " + path.string());
  }

  const auto end_position = input.tellg();
  if (end_position < 0) {
    throw std::runtime_error("Failed to determine test fixture size: " + path.string());
  }

  std::vector<uint8_t> bytes(static_cast<std::size_t>(end_position));
  input.seekg(0, std::ios::beg);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  if (!input) {
    throw std::runtime_error("Failed to read test fixture: " + path.string());
  }
  return bytes;
}

}  // namespace pj::parser_arrow::test
