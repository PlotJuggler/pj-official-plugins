// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <pj_laser_scan/laser_scan_projector.hpp>
#include <vector>

namespace {

using PJ::laser_scan::LaserScanProjector;
using PJ::laser_scan::ScanParams;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// Reads the float at byte offset `off` of point `index` in a packed cloud.
float pointFloat(const PJ::sdk::PointCloud& cloud, uint32_t index, uint32_t off) {
  float v = 0.0f;
  std::memcpy(&v, cloud.data.data() + static_cast<size_t>(index) * cloud.point_step + off, sizeof(float));
  return v;
}

/// The reference projection the LUT must match bit-for-bit: theta in double
/// precision, cos/sin in double cast to float, multiply in float.
float naiveX(double angle_min, double angle_increment, size_t i, float range) {
  return range * static_cast<float>(std::cos(angle_min + static_cast<double>(i) * angle_increment));
}
float naiveY(double angle_min, double angle_increment, size_t i, float range) {
  return range * static_cast<float>(std::sin(angle_min + static_cast<double>(i) * angle_increment));
}

PJ::Span<const float> span(const std::vector<float>& v) {
  return PJ::Span<const float>(v.data(), v.size());
}
PJ::Span<const double> span(const std::vector<double>& v) {
  return PJ::Span<const double>(v.data(), v.size());
}

TEST(LaserScanProjectorTest, LutMatchesNaiveTrig) {
  LaserScanProjector projector;
  const ScanParams params{.angle_min = -1.5, .angle_increment = 0.01};
  const std::vector<float> ranges = {1.0f, 2.5f, 0.75f, 10.0f, 3.25f};

  const auto cloud = projector.project(params, span(ranges), {});

  ASSERT_EQ(cloud.width, ranges.size());
  EXPECT_EQ(cloud.height, 1u);
  EXPECT_EQ(cloud.point_step, 12u);
  EXPECT_EQ(cloud.row_step, cloud.width * cloud.point_step);
  EXPECT_TRUE(cloud.is_dense);
  EXPECT_FALSE(cloud.is_bigendian);

  ASSERT_EQ(cloud.fields.size(), 3u);
  EXPECT_EQ(cloud.fields[0].name, "x");
  EXPECT_EQ(cloud.fields[0].offset, 0u);
  EXPECT_EQ(cloud.fields[0].datatype, PJ::sdk::PointField::Datatype::kFloat32);
  EXPECT_EQ(cloud.fields[1].name, "y");
  EXPECT_EQ(cloud.fields[1].offset, 4u);
  EXPECT_EQ(cloud.fields[2].name, "z");
  EXPECT_EQ(cloud.fields[2].offset, 8u);

  for (size_t i = 0; i < ranges.size(); ++i) {
    const auto idx = static_cast<uint32_t>(i);
    // Exact match: the LUT path must compute the identical float as the naive
    // per-ray trig reference.
    EXPECT_EQ(pointFloat(cloud, idx, 0), naiveX(params.angle_min, params.angle_increment, i, ranges[i])) << "ray " << i;
    EXPECT_EQ(pointFloat(cloud, idx, 4), naiveY(params.angle_min, params.angle_increment, i, ranges[i])) << "ray " << i;
    EXPECT_EQ(pointFloat(cloud, idx, 8), 0.0f) << "ray " << i;
  }
}

TEST(LaserScanProjectorTest, LutReusedAcrossIdenticalScans) {
  LaserScanProjector projector;
  const ScanParams params{.angle_min = 0.0, .angle_increment = 0.005};
  const std::vector<float> ranges(360, 2.0f);

  EXPECT_EQ(projector.lutRebuildCount(), 0u);
  (void)projector.project(params, span(ranges), {});
  EXPECT_EQ(projector.lutRebuildCount(), 1u);

  // Same (ray_count, angle_min, angle_increment) key: the LUT must be reused
  // for a whole recording of a fixed scanner config.
  for (int i = 0; i < 10; ++i) {
    (void)projector.project(params, span(ranges), {});
  }
  EXPECT_EQ(projector.lutRebuildCount(), 1u);
}

TEST(LaserScanProjectorTest, LutInvalidatedOnKeyChange) {
  LaserScanProjector projector;
  const std::vector<float> ranges(8, 1.0f);

  (void)projector.project({.angle_min = 0.0, .angle_increment = 0.1}, span(ranges), {});
  EXPECT_EQ(projector.lutRebuildCount(), 1u);

  // angle_min changes -> rebuild.
  (void)projector.project({.angle_min = 0.5, .angle_increment = 0.1}, span(ranges), {});
  EXPECT_EQ(projector.lutRebuildCount(), 2u);

  // angle_increment changes -> rebuild.
  (void)projector.project({.angle_min = 0.5, .angle_increment = 0.2}, span(ranges), {});
  EXPECT_EQ(projector.lutRebuildCount(), 3u);

  // ray count changes -> rebuild.
  const std::vector<float> more_rays(16, 1.0f);
  (void)projector.project({.angle_min = 0.5, .angle_increment = 0.2}, span(more_rays), {});
  EXPECT_EQ(projector.lutRebuildCount(), 4u);

  // Unchanged key after the churn -> no rebuild.
  (void)projector.project({.angle_min = 0.5, .angle_increment = 0.2}, span(more_rays), {});
  EXPECT_EQ(projector.lutRebuildCount(), 4u);

  // Range bounds are NOT part of the key: changing them must not rebuild.
  (void)projector.project(
      {.angle_min = 0.5, .angle_increment = 0.2, .range_min = 0.1, .range_max = 5.0}, span(more_rays), {});
  EXPECT_EQ(projector.lutRebuildCount(), 4u);
}

TEST(LaserScanProjectorTest, DropsNonFiniteAndOutOfRangeRays) {
  LaserScanProjector projector;
  const ScanParams params{.angle_min = 0.0, .angle_increment = 0.1, .range_min = 0.5, .range_max = 10.0};
  // Kept: 1.0 (i=0), 0.5 (=min, i=3), 10.0 (=max, i=5), 2.0 (i=7).
  const std::vector<float> ranges = {1.0f, kNaN, kInf, 0.5f, 0.4f, 10.0f, 10.5f, 2.0f, -kInf};

  const auto cloud = projector.project(params, span(ranges), {});

  ASSERT_EQ(cloud.width, 4u);
  EXPECT_EQ(cloud.row_step, 4u * 12u);
  EXPECT_TRUE(cloud.is_dense);  // invalid rays were dropped, not NaN-filled
  ASSERT_EQ(cloud.data.size(), 4u * 12u);

  // Kept points carry the angle of their ORIGINAL ray index.
  EXPECT_EQ(pointFloat(cloud, 0, 0), naiveX(params.angle_min, params.angle_increment, 0, 1.0f));
  EXPECT_EQ(pointFloat(cloud, 1, 0), naiveX(params.angle_min, params.angle_increment, 3, 0.5f));
  EXPECT_EQ(pointFloat(cloud, 2, 1 * 4), naiveY(params.angle_min, params.angle_increment, 5, 10.0f));
  EXPECT_EQ(pointFloat(cloud, 3, 0), naiveX(params.angle_min, params.angle_increment, 7, 2.0f));
}

TEST(LaserScanProjectorTest, NoBoundsKeepAllFiniteRays) {
  LaserScanProjector projector;
  // No range_min/range_max (the foxglove route): only non-finite rays drop.
  const ScanParams params{.angle_min = 0.0, .angle_increment = 0.1};
  const std::vector<float> ranges = {0.001f, kNaN, 1000.0f, kInf};

  const auto cloud = projector.project(params, span(ranges), {});
  EXPECT_EQ(cloud.width, 2u);
}

TEST(LaserScanProjectorTest, IntensityPassthrough) {
  LaserScanProjector projector;
  const ScanParams params{.angle_min = -0.2, .angle_increment = 0.05, .range_min = 0.5, .range_max = 10.0};
  const std::vector<float> ranges = {1.0f, kNaN, 3.0f};
  const std::vector<float> intensities = {100.0f, 200.0f, 300.0f};

  const auto cloud = projector.project(params, span(ranges), span(intensities));

  ASSERT_EQ(cloud.width, 2u);
  EXPECT_EQ(cloud.point_step, 16u);
  EXPECT_EQ(cloud.row_step, 2u * 16u);
  ASSERT_EQ(cloud.fields.size(), 4u);
  EXPECT_EQ(cloud.fields[3].name, "intensity");
  EXPECT_EQ(cloud.fields[3].offset, 12u);
  EXPECT_EQ(cloud.fields[3].datatype, PJ::sdk::PointField::Datatype::kFloat32);

  // Intensity follows its ray through the drop filter.
  EXPECT_EQ(pointFloat(cloud, 0, 12), 100.0f);
  EXPECT_EQ(pointFloat(cloud, 1, 12), 300.0f);
}

TEST(LaserScanProjectorTest, XyzOnlyWhenIntensitiesSizeMismatched) {
  LaserScanProjector projector;
  const ScanParams params{.angle_min = 0.0, .angle_increment = 0.1};
  const std::vector<float> ranges = {1.0f, 2.0f, 3.0f};
  const std::vector<float> intensities = {7.0f};  // wrong size -> ignored

  const auto cloud = projector.project(params, span(ranges), span(intensities));

  EXPECT_EQ(cloud.point_step, 12u);
  EXPECT_EQ(cloud.fields.size(), 3u);
  EXPECT_EQ(cloud.width, 3u);
}

TEST(LaserScanProjectorTest, XyzOnlyWhenIntensitiesAbsent) {
  LaserScanProjector projector;
  const auto cloud = projector.project({.angle_min = 0.0, .angle_increment = 0.1}, span(std::vector<float>{1.0f}), {});
  EXPECT_EQ(cloud.point_step, 12u);
  EXPECT_EQ(cloud.fields.size(), 3u);
}

TEST(LaserScanProjectorTest, EmptyScanProducesEmptyCloud) {
  LaserScanProjector projector;
  const auto cloud = projector.project({.angle_min = 0.0, .angle_increment = 0.1}, PJ::Span<const float>{}, {});
  EXPECT_EQ(cloud.width, 0u);
  EXPECT_EQ(cloud.height, 1u);
  EXPECT_EQ(cloud.row_step, 0u);
  EXPECT_EQ(cloud.data.size(), 0u);
  EXPECT_TRUE(cloud.is_dense);
  EXPECT_EQ(cloud.fields.size(), 3u);  // layout still well-formed
}

TEST(LaserScanProjectorTest, AnchorOwnsGeneratedBuffer) {
  // The point bytes are newly generated (not wire-aliased); the anchor must
  // keep them alive independently of the projector.
  PJ::sdk::PointCloud cloud;
  {
    LaserScanProjector projector;
    const std::vector<float> ranges = {2.0f};
    cloud = projector.project({.angle_min = 0.5, .angle_increment = 0.0}, span(ranges), {});
  }
  ASSERT_NE(cloud.anchor, nullptr);
  ASSERT_EQ(cloud.data.size(), 12u);
  EXPECT_EQ(pointFloat(cloud, 0, 0), 2.0f * static_cast<float>(std::cos(0.5)));
  EXPECT_EQ(pointFloat(cloud, 0, 4), 2.0f * static_cast<float>(std::sin(0.5)));
}

TEST(LaserScanProjectorTest, DoubleOverloadMatchesFloatProjection) {
  // The foxglove route feeds doubles; geometry must match the float route on
  // values representable in both.
  LaserScanProjector projector;
  const ScanParams params{.angle_min = -0.7, .angle_increment = 0.02};
  const std::vector<double> dranges = {1.5, std::numeric_limits<double>::quiet_NaN(), 4.25};
  const std::vector<double> dintens = {10.0, 20.0, 30.0};

  const auto cloud = projector.project(params, span(dranges), span(dintens));

  ASSERT_EQ(cloud.width, 2u);
  EXPECT_EQ(cloud.point_step, 16u);
  EXPECT_EQ(pointFloat(cloud, 0, 0), naiveX(params.angle_min, params.angle_increment, 0, 1.5f));
  EXPECT_EQ(pointFloat(cloud, 1, 4), naiveY(params.angle_min, params.angle_increment, 2, 4.25f));
  EXPECT_EQ(pointFloat(cloud, 0, 12), 10.0f);
  EXPECT_EQ(pointFloat(cloud, 1, 12), 30.0f);
}

}  // namespace
