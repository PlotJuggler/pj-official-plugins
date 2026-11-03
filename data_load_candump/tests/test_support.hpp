#pragma once

// Tiny bit of scaffolding shared by every candump test file that reads a
// fixture from test_data/ -- CANDUMP_TEST_DATA_DIR is set per test binary by
// CMakeLists.txt's target_compile_definitions().

#include <filesystem>
#include <string>

namespace candump_test {

inline std::string testDataPath(const char* name) {
  return (std::filesystem::path(CANDUMP_TEST_DATA_DIR) / name).string();
}

}  // namespace candump_test
