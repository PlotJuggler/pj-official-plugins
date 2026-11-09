#include "socketcan_link.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using can_stream::socketCanLinkProblem;

// A fake /sys/class/net with one interface whose flags file holds `flags`.
class SocketCanLink : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("pj_socketcan_link_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    fs::remove_all(root_);
  }
  void TearDown() override {
    fs::remove_all(root_);
  }
  void writeFlags(const std::string& iface, const std::string& flags) {
    fs::create_directories(root_ / iface);
    std::ofstream(root_ / iface / "flags") << flags << "\n";
  }
  fs::path root_;
};

TEST_F(SocketCanLink, UpInterfaceIsFine) {
  writeFlags("vcan0", "0x1003");  // IFF_UP (0x1) set
  EXPECT_EQ(socketCanLinkProblem(root_.string(), "vcan0"), std::nullopt);
}

TEST_F(SocketCanLink, DownInterfaceWentDown) {
  writeFlags("vcan0", "0x1002");
  EXPECT_EQ(socketCanLinkProblem(root_.string(), "vcan0"), "interface went down");
}

TEST_F(SocketCanLink, MissingInterfaceWasRemoved) {
  writeFlags("vcan0", "0x1003");
  EXPECT_EQ(socketCanLinkProblem(root_.string(), "can1"), "interface removed");
}

}  // namespace
