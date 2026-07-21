#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../mf4_reader.hpp"

// Real-file integration tests. The fixtures (real automotive logs) are not
// committed; point MF4_TEST_DATA_DIR at a directory containing
// ASAP2_Demo_V171.mf4 and canedge_00000001.MF4 to run these. Skipped otherwise.
namespace {

using mf4_detail::Mf4Reader;
using mf4_detail::SampleValue;

std::string fixture(const char* name) {
  const char* dir = std::getenv("MF4_TEST_DATA_DIR");
  if (dir == nullptr || *dir == '\0') {
    return {};
  }
  const auto path = std::filesystem::path(dir) / name;
  std::error_code ec;
  return std::filesystem::exists(path, ec) ? path.string() : std::string{};
}

TEST(Mf4Integration, ReadsFinalizedAsap2Demo) {
  const std::string path = fixture("ASAP2_Demo_V171.mf4");
  if (path.empty()) {
    GTEST_SKIP() << "set MF4_TEST_DATA_DIR to a dir containing ASAP2_Demo_V171.mf4";
  }

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  EXPECT_TRUE(reader.finalized());
  EXPECT_GT(reader.startTimeNs(), 0);
  EXPECT_EQ(reader.groups().size(), 7u);  // known structure of the ASAP2 demo

  std::size_t total_rows = 0;
  std::size_t total_series = 0;
  for (std::size_t gi = 0; gi < reader.groups().size(); ++gi) {
    const auto& g = reader.groups()[gi];
    if (!g.has_master || g.sample_count == 0) {
      continue;
    }
    total_series += reader.valueChannelNames(gi).size();
    const auto status = reader.readGroup(gi, [&](std::int64_t, const std::vector<SampleValue>&) {
      ++total_rows;
      return true;
    });
    EXPECT_TRUE(status.has_value());
  }
  EXPECT_GT(total_rows, 0u);
  EXPECT_GT(total_series, 0u);
}

TEST(Mf4Integration, ReadsUnfinalizedCanedgeFrames) {
  const std::string path = fixture("canedge_00000001.MF4");
  if (path.empty()) {
    GTEST_SKIP() << "set MF4_TEST_DATA_DIR to a dir containing canedge_00000001.MF4";
  }

  Mf4Reader reader;
  // The CANedge file has the UnFinMF (unfinalized) marker; mdflib reads it anyway.
  ASSERT_TRUE(reader.open(path).has_value());

  std::size_t can_frames = 0;
  bool saw_extended = false;
  for (std::size_t gi = 0; gi < reader.groups().size(); ++gi) {
    const auto& g = reader.groups()[gi];
    if (g.bus_type != 2 || g.sample_count == 0) {  // 2 == CAN
      continue;
    }
    const auto status = reader.readCanGroup(
        gi, [&](std::int64_t, std::uint16_t, std::uint32_t, bool extended, const std::vector<std::uint8_t>& data) {
          ++can_frames;
          saw_extended = saw_extended || extended;
          EXPECT_FALSE(data.empty());
          return true;
        });
    EXPECT_TRUE(status.has_value());
  }
  EXPECT_GT(can_frames, 0u);
  EXPECT_TRUE(saw_extended);  // the canedge log carries 29-bit J1939 frames
}

}  // namespace
