// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <vector>

#include "pj_plugins/host/data_source_library.hpp"

// The proxy's internal load operation accepts the resolved installation paths,
// allowing real linker failures against isolated test trees without ROS.
#include "../proxy/src/data_stream_ros2.cpp"

namespace {

class Ros2ProxyTest : public testing::Test {
 protected:
  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "pj-ros2-proxy-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const auto* dir = mkdtemp(buffer.data());
    ASSERT_NE(dir, nullptr);
    root_ = dir;
    g_data_source_vtable = nullptr;
    g_dialog_vtable = nullptr;
    g_last_error.clear();
    g_inner_handle = nullptr;
  }

  void TearDown() override {
    if (g_inner_handle != nullptr) {
      dlclose(g_inner_handle);
      g_inner_handle = nullptr;
    }
    std::filesystem::remove_all(root_);
  }

  void stageInner(const std::filesystem::path& source, bool robostack = false) const {
    const auto target =
        root_ / "plugin/dist" / (robostack ? "jazzy-robostack" : "jazzy") / "libros2_stream_plugin-jazzy.pjros2";
    std::filesystem::create_directories(target.parent_path());
    std::filesystem::copy_file(source, target);
  }

  void load(bool robostack = false) const {
    loadInnerImpl(std::string("jazzy"), root_ / "plugin", robostack, root_ / "ros");
  }

  void expectUnavailable(const std::string& instruction) const {
    ASSERT_EQ(g_data_source_vtable, &kUnavailableVtable);
    auto library = PJ::DataSourceLibrary::loadStatic(g_data_source_vtable);
    ASSERT_TRUE(library) << library.error();
    auto handle = library->createHandle();
    ASSERT_TRUE(handle.valid());
    EXPECT_EQ(handle.capabilities(), PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM);
    auto second_handle = library->createHandle();
    ASSERT_TRUE(second_handle.valid());
    const auto bound = handle.bind({});
    ASSERT_FALSE(bound);
    EXPECT_NE(bound.error().find(instruction), std::string::npos);
    EXPECT_EQ(handle.currentState(), PJ::DataSourceState::kFailed);
  }

  std::filesystem::path root_;
};

TEST_F(Ros2ProxyTest, NoRosInstallationAdmitsSourceAndReportsSetupOnExplicitBind) {
  loadInnerImpl(std::nullopt, root_ / "plugin", false, {});
  expectUnavailable("source /opt/ros/<distro>/setup.bash");
}

TEST_F(Ros2ProxyTest, UnsourcedInstalledRosAdmitsSourceAndReportsSelectedSetup) {
  stageInner(ROS2_TEST_INNER);
  std::filesystem::create_directories(root_ / "ros/lib");
  std::filesystem::copy_file(
      ROS2_TEST_DEPENDENCY, root_ / "ros/lib" / std::filesystem::path(ROS2_TEST_DEPENDENCY).filename());
  load();
  expectUnavailable("source " + (root_ / "ros/setup.bash").string());
}

TEST_F(Ros2ProxyTest, MissingInstalledDependencyRemainsFatal) {
  stageInner(ROS2_TEST_INNER);
  load();
  EXPECT_EQ(g_data_source_vtable, nullptr);
  EXPECT_NE(g_last_error.find("dlopen failed"), std::string::npos);
}

TEST_F(Ros2ProxyTest, RoboStackLoaderFaultRemainsFatalEvenWithLibraryPresent) {
  stageInner(ROS2_TEST_INNER, true);
  std::filesystem::create_directories(root_ / "ros/lib");
  std::filesystem::copy_file(
      ROS2_TEST_DEPENDENCY, root_ / "ros/lib" / std::filesystem::path(ROS2_TEST_DEPENDENCY).filename());
  load(true);
  EXPECT_EQ(g_data_source_vtable, nullptr);
  EXPECT_NE(g_last_error.find("dlopen failed"), std::string::npos);
}

TEST_F(Ros2ProxyTest, MissingPayloadRemainsFatal) {
  load();
  EXPECT_EQ(g_data_source_vtable, nullptr);
  EXPECT_NE(g_last_error.find("no matching binary"), std::string::npos);
}

TEST_F(Ros2ProxyTest, UnsupportedDistroRemainsFatal) {
  loadInnerImpl(std::string("iron"), root_ / "plugin", false, root_ / "ros");
  EXPECT_EQ(g_data_source_vtable, nullptr);
  EXPECT_NE(g_last_error.find("'iron'"), std::string::npos);
}

TEST_F(Ros2ProxyTest, MissingInnerExportsRemainFatal) {
  stageInner(ROS2_TEST_MISSING_EXPORTS);
  load();
  EXPECT_EQ(g_data_source_vtable, nullptr);
  EXPECT_NE(g_last_error.find("dlsym(PJ_ros2_inner_get_data_source_vtable) failed"), std::string::npos);
}

TEST_F(Ros2ProxyTest, CorruptPayloadRemainsFatal) {
  const auto corrupt = root_ / "corrupt";
  std::ofstream(corrupt) << "invalid ELF";
  stageInner(corrupt);
  load();
  EXPECT_EQ(g_data_source_vtable, nullptr);
  EXPECT_NE(g_last_error.find("dlopen failed"), std::string::npos);
}

// RoboStack ships rclcpp as ros2-rclcpp (lyrical; jazzy/kilted on macOS); only
// ros2-distro-mutex's build string names the distro.
TEST_F(Ros2ProxyTest, CondaDistroDetectedFromDistroMutex) {
  const auto meta = root_ / "env/conda-meta";
  std::filesystem::create_directories(meta);
  std::ofstream(meta / "ros2-rclcpp-32.0.3-np2py314h53a0733_30.json") << "{}";
  std::ofstream(meta / "ros2-distro-mutex-0.22.0-lyrical_30.json") << "{}";
  EXPECT_TRUE(condaHasDistro(root_ / "env", "lyrical"));
  EXPECT_FALSE(condaHasDistro(root_ / "env", "jazzy"));
}

TEST_F(Ros2ProxyTest, CondaDistroDetectedFromLegacyRclcppRecord) {
  const auto meta = root_ / "env/conda-meta";
  std::filesystem::create_directories(meta);
  std::ofstream(meta / "ros-jazzy-rclcpp-28.1.21-np2py312h2ed9cc7_21.json") << "{}";
  EXPECT_TRUE(condaHasDistro(root_ / "env", "jazzy"));
  EXPECT_FALSE(condaHasDistro(root_ / "env", "kilted"));
}

TEST_F(Ros2ProxyTest, SymbolAndVersionErrorsAreNeverClassifiedAsMissingSearchPath) {
  const auto lib = root_ / "ros/lib/libstdc++.so.6";
  std::filesystem::create_directories(lib.parent_path());
  std::ofstream(lib) << "present";
  EXPECT_FALSE(missingRosSearchPath("libstdc++.so.6: version `GLIBCXX_3.4.32' not found", root_ / "ros"));
  EXPECT_FALSE(missingRosSearchPath("payload: undefined symbol: required_symbol", root_ / "ros"));
  EXPECT_FALSE(missingRosSearchPath(
      "../libstdc++.so.6: cannot open shared object file: No such file or directory", root_ / "ros"));
}

}  // namespace
