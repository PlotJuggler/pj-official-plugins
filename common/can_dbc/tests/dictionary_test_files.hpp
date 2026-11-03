#pragma once

// Host-free temp-file helpers shared by the dictionary-path tests: the
// pj_can_dbc unit test and (through source_import_harness.hpp) the layout-path
// tests of data_load_blf / data_load_candump / data_load_mf4.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace pj_can_dbc::testing {

namespace fs = std::filesystem;

/// Temp directory unique to the running test, removed (best effort) on destruction.
class ScratchDir {
 public:
  ScratchDir() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    root_ =
        fs::temp_directory_path() / (std::string("pj_layout_paths_") + info->test_suite_name() + "_" + info->name());
    std::error_code ignored;
    fs::remove_all(root_, ignored);
    fs::create_directories(root_);
  }
  ~ScratchDir() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  const fs::path& root() const {
    return root_;
  }

 private:
  fs::path root_;
};

/// UTF-8 text with '/' separators, for JSON configs and comparisons.
inline std::string generic(const fs::path& path) {
  const auto text = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// Writes a one-message DBC: id 256 named `message_name`, `dlc` bytes, one 16-bit signal `Speed`.
inline void writeEngineDbc(const fs::path& file, const std::string& message_name, int dlc) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary);
  out << "VERSION \"1.0.0\"\n\nNS_ :\n\nBS_:\n\nBU_: ECU\n\n"
      << "BO_ 256 " << message_name << ": " << dlc << " ECU\n"
      << " SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] \"km/h\" ECU\n";
}

}  // namespace pj_can_dbc::testing
