// SPDX-License-Identifier: MIT
#include "ply_reader.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

PJ::Span<const uint8_t> asSpan(const std::string& s) {
  return PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void putF32LE(std::string& s, float v) {
  char b[4];
  std::memcpy(b, &v, 4);
  s.append(b, 4);
}

TEST(PlyReader, AsciiVerticesToCloud) {
  std::string ply =
      "ply\nformat ascii 1.0\nelement vertex 2\n"
      "property float x\nproperty float y\nproperty float z\n"
      "end_header\n1 2 3\n4 5 6\n";
  auto r = pj3d::readPly(asSpan(ply), "mesh_or_cloud");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  ASSERT_TRUE(r->cloud.has_value());
  EXPECT_FALSE(r->mesh.has_value());
  EXPECT_EQ(r->cloud->width, 2u);
  EXPECT_EQ(r->cloud->fields.size(), 3u);
  float x1 = 0.0f;
  std::memcpy(&x1, r->cloud->data.data() + 12u, 4);
  EXPECT_FLOAT_EQ(x1, 4.0f);
}

TEST(PlyReader, BinaryLittleEndianVerticesToCloud) {
  std::string ply =
      "ply\nformat binary_little_endian 1.0\nelement vertex 2\n"
      "property float x\nproperty float y\nproperty float z\nend_header\n";
  putF32LE(ply, 1.0f);
  putF32LE(ply, 2.0f);
  putF32LE(ply, 3.0f);
  putF32LE(ply, 4.0f);
  putF32LE(ply, 5.0f);
  putF32LE(ply, 6.0f);
  auto r = pj3d::readPly(asSpan(ply), "c");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  ASSERT_TRUE(r->cloud.has_value());
  float z1 = 0.0f;
  std::memcpy(&z1, r->cloud->data.data() + 12u + 8u, 4);
  EXPECT_FLOAT_EQ(z1, 6.0f);
}

TEST(PlyReader, FacesRouteToMesh) {
  std::string ply =
      "ply\nformat ascii 1.0\nelement vertex 3\n"
      "property float x\nproperty float y\nproperty float z\n"
      "element face 1\nproperty list uchar int vertex_indices\n"
      "end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";
  auto r = pj3d::readPly(asSpan(ply), "m");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  ASSERT_TRUE(r->mesh.has_value());
  EXPECT_FALSE(r->cloud.has_value());
  EXPECT_EQ(r->mesh->format, "ply");
  EXPECT_EQ(r->mesh->frame_id, "m");
  EXPECT_EQ(r->num_faces, 1u);
  // Raw bytes are the whole file, untouched.
  EXPECT_EQ(r->mesh->data.size(), ply.size());
  EXPECT_EQ(std::memcmp(r->mesh->data.data(), ply.data(), ply.size()), 0);
}

TEST(PlyReader, VertexColorAndNormalPreserved) {
  std::string ply =
      "ply\nformat ascii 1.0\nelement vertex 1\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property uchar red\nproperty uchar green\nproperty uchar blue\n"
      "end_header\n0 0 0 255 128 64\n";
  auto r = pj3d::readPly(asSpan(ply), "c");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  ASSERT_TRUE(r->cloud.has_value());
  ASSERT_EQ(r->cloud->fields.size(), 6u);
  EXPECT_EQ(r->cloud->fields[3].name, "red");
  EXPECT_EQ(r->cloud->fields[3].datatype, pj3d::Datatype::kUint8);
  EXPECT_EQ(r->cloud->point_step, 12u + 3u);
}

// Proves the mesh is self-owning: destroy the input buffer, mesh bytes survive
// AND still compare equal to the original content (reads mesh.data.data(),
// not just .size(), so an aliasing bug would be caught).
TEST(PlyReader, MeshSurvivesInputDestruction) {
  PJ::sdk::Mesh3D mesh;
  std::string original;  // keep a copy of the expected bytes past the input's scope
  {
    std::string ply =
        "ply\nformat ascii 1.0\nelement vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_indices\n"
        "end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";
    original = ply;  // independent copy of the expected content
    auto r = pj3d::readPly(asSpan(ply), "m");
    ASSERT_TRUE(r) << (r ? "" : r.error());
    ASSERT_TRUE(r->mesh.has_value());
    mesh = *r->mesh;  // shares ownership via anchor
  }  // input `ply` and `r` destroyed here; if mesh aliased `ply`, data() now dangles
  ASSERT_EQ(mesh.data.size(), original.size());
  ASSERT_NE(mesh.data.data(), nullptr);
  EXPECT_EQ(std::memcmp(mesh.data.data(), original.data(), original.size()), 0);
  EXPECT_EQ(mesh.format, "ply");
}

TEST(PlyReader, MissingEndHeaderIsCleanError) {
  std::string ply = "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n";  // no end_header
  auto r = pj3d::readPly(asSpan(ply), "c");
  EXPECT_FALSE(r);
}

TEST(PlyReader, UnknownVertexPropertyTypeIsCleanError) {
  std::string ply =
      "ply\nformat ascii 1.0\nelement vertex 1\n"
      "property weird x\nend_header\n0\n";
  auto r = pj3d::readPly(asSpan(ply), "c");
  EXPECT_FALSE(r);
}

TEST(PlyReader, BinaryBigEndianVerticesToCloud) {
  std::string ply =
      "ply\nformat binary_big_endian 1.0\nelement vertex 1\n"
      "property float x\nproperty float y\nproperty float z\nend_header\n";
  const unsigned char be[12] = {0x3F, 0x80, 0x00, 0x00,   // x = 1.0f BE
                                0x40, 0x00, 0x00, 0x00,   // y = 2.0f BE
                                0x40, 0x40, 0x00, 0x00};  // z = 3.0f BE
  ply.append(reinterpret_cast<const char*>(be), 12);
  auto r = pj3d::readPly(asSpan(ply), "c");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  ASSERT_TRUE(r->cloud.has_value());
  float x = 0.0f, z = 0.0f;
  std::memcpy(&x, r->cloud->data.data() + 0u, 4);
  std::memcpy(&z, r->cloud->data.data() + 8u, 4);
  EXPECT_FLOAT_EQ(x, 1.0f);
  EXPECT_FLOAT_EQ(z, 3.0f);
}

TEST(PlyReader, VertexCountOverflowIsCleanError) {
  // 4294967296 == UINT32_MAX + 1 -> guarded before the uint32 narrowing.
  std::string ply =
      "ply\nformat ascii 1.0\nelement vertex 4294967296\n"
      "property float x\nend_header\n";
  auto r = pj3d::readPly(asSpan(ply), "c");
  EXPECT_FALSE(r);
}

TEST(PlyReader, UnknownFormatIsCleanError) {
  std::string ply = "ply\nformat weird 1.0\nelement vertex 1\nproperty float x\nend_header\n0\n";
  auto r = pj3d::readPly(asSpan(ply), "c");
  EXPECT_FALSE(r);
}

TEST(PlyReader, NoVertexPropertiesIsCleanError) {
  std::string ply = "ply\nformat ascii 1.0\nelement vertex 1\nend_header\n";
  auto r = pj3d::readPly(asSpan(ply), "c");
  EXPECT_FALSE(r);
}

TEST(PlyReader, ListPropertyInVertexIsCleanError) {
  std::string ply =
      "ply\nformat ascii 1.0\nelement vertex 1\n"
      "property list uchar int weird\nend_header\n";
  auto r = pj3d::readPly(asSpan(ply), "c");
  EXPECT_FALSE(r);
}

TEST(PlyReader, AsciiCrlfHeaderVerticesToCloud) {
  std::string ply =
      "ply\r\nformat ascii 1.0\r\nelement vertex 2\r\n"
      "property float x\r\nproperty float y\r\nproperty float z\r\n"
      "end_header\r\n1 2 3\r\n4 5 6\r\n";
  auto r = pj3d::readPly(asSpan(ply), "c");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  ASSERT_TRUE(r->cloud.has_value());
  EXPECT_EQ(r->cloud->width, 2u);
  float x1 = 0.0f;
  std::memcpy(&x1, r->cloud->data.data() + 12u, 4);
  EXPECT_FLOAT_EQ(x1, 4.0f);
}

TEST(PlyReader, MeshWithListVertexPropertyRoutesToMesh) {
  // A mesh file (has faces) whose vertex element declares a list property must
  // STILL route to the raw-bytes Mesh3D path (regression guard): vertex-property
  // quirks don't block the mesh route.
  std::string ply =
      "ply\nformat ascii 1.0\nelement vertex 1\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property list uchar int custom\n"
      "element face 1\nproperty list uchar int vertex_indices\n"
      "end_header\n0 0 0 0\n3 0 0 0\n";
  auto r = pj3d::readPly(asSpan(ply), "m");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  EXPECT_TRUE(r->mesh.has_value());
  EXPECT_FALSE(r->cloud.has_value());
  EXPECT_EQ(r->num_faces, 1u);
}

TEST(PlyReader, MissingPlyMagicIsCleanError) {
  std::string ply = "format ascii 1.0\nelement vertex 1\nproperty float x\nend_header\n0\n";
  auto r = pj3d::readPly(asSpan(ply), "c");
  EXPECT_FALSE(r);
}

}  // namespace
