#pragma once

// CANDUMP_TEST_DATA_DIR is set per test binary by CMakeLists.txt.

#include <filesystem>
#include <string>

namespace candump_test {

inline std::string testDataPath(const char* name) {
  return (std::filesystem::path(CANDUMP_TEST_DATA_DIR) / name).string();
}

}  // namespace candump_test
