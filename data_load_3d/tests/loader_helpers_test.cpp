// SPDX-License-Identifier: MIT
#include "loader_helpers.hpp"

#include <gtest/gtest.h>

namespace {

TEST(LoaderHelpers, ObjectMetadataIsBuiltinObjectTypeKey) {
  // This exact JSON is what PJ4 classifies on to render the object.
  EXPECT_EQ(
      pj3d::builtinObjectMetadata(PJ::sdk::BuiltinObjectType::kPointCloud), R"({"builtin_object_type":"kPointCloud"})");
  EXPECT_EQ(pj3d::builtinObjectMetadata(PJ::sdk::BuiltinObjectType::kMesh3D), R"({"builtin_object_type":"kMesh3D"})");
}

TEST(LoaderHelpers, LowerExtension) {
  EXPECT_EQ(pj3d::lowerExtension("/a/b/foo.pcd"), ".pcd");
  EXPECT_EQ(pj3d::lowerExtension("/a/b/foo.PLY"), ".ply");  // lowercased
  EXPECT_EQ(pj3d::lowerExtension("relative.PcD"), ".pcd");
  EXPECT_EQ(pj3d::lowerExtension("/a/b/noext"), "");  // no dot
  EXPECT_EQ(pj3d::lowerExtension("/a.b/c"), "");      // dot only in a parent dir
  EXPECT_EQ(pj3d::lowerExtension(".pcd"), ".pcd");    // dotfile: extension is ".pcd"
}

TEST(LoaderHelpers, FileStem) {
  EXPECT_EQ(pj3d::fileStem("/a/b/foo.pcd"), "foo");
  EXPECT_EQ(pj3d::fileStem("foo.ply"), "foo");
  EXPECT_EQ(pj3d::fileStem("/a/b/noext"), "noext");
  EXPECT_EQ(pj3d::fileStem("/tmp/.pcd"), ".pcd");         // dotfile -> whole basename, never ""
  EXPECT_EQ(pj3d::fileStem("scan.bin.pcd"), "scan.bin");  // strips only the last extension
}

}  // namespace
