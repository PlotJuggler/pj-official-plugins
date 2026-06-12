// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#include "pj_laser_scan/laser_scan_projector.hpp"

#include <cmath>
#include <memory>

namespace PJ {
namespace laser_scan {

namespace {

/// x/y/z FLOAT32 point layout; +4 bytes when intensity passes through.
constexpr uint32_t kXyzStep = 12;
constexpr uint32_t kXyziStep = 16;

std::vector<sdk::PointField> makeFields(bool with_intensity) {
  using Datatype = sdk::PointField::Datatype;
  std::vector<sdk::PointField> fields = {
      {.name = "x", .offset = 0, .datatype = Datatype::kFloat32, .count = 1},
      {.name = "y", .offset = 4, .datatype = Datatype::kFloat32, .count = 1},
      {.name = "z", .offset = 8, .datatype = Datatype::kFloat32, .count = 1},
  };
  if (with_intensity) {
    fields.push_back({.name = "intensity", .offset = 12, .datatype = Datatype::kFloat32, .count = 1});
  }
  return fields;
}

}  // namespace

sdk::PointCloud LaserScanProjector::project(
    const ScanParams& params, Span<const float> ranges, Span<const float> intensities) {
  return projectImpl<float>(params, ranges, intensities);
}

sdk::PointCloud LaserScanProjector::project(
    const ScanParams& params, Span<const double> ranges, Span<const double> intensities) {
  return projectImpl<double>(params, ranges, intensities);
}

void LaserScanProjector::ensureLut(size_t ray_count, double angle_min, double angle_increment) {
  if (ray_count == lut_ray_count_ && angle_min == lut_angle_min_ && angle_increment == lut_angle_increment_) {
    return;
  }
  cos_lut_.resize(ray_count);
  sin_lut_.resize(ray_count);
  for (size_t i = 0; i < ray_count; ++i) {
    // Double-precision angle, single-precision trig result: the LUT must
    // reproduce exactly what naive per-ray trig would compute in float.
    const double theta = angle_min + static_cast<double>(i) * angle_increment;
    cos_lut_[i] = static_cast<float>(std::cos(theta));
    sin_lut_[i] = static_cast<float>(std::sin(theta));
  }
  lut_ray_count_ = ray_count;
  lut_angle_min_ = angle_min;
  lut_angle_increment_ = angle_increment;
  ++lut_rebuild_count_;
}

template <typename Scalar>
sdk::PointCloud LaserScanProjector::projectImpl(
    const ScanParams& params, Span<const Scalar> ranges, Span<const Scalar> intensities) {
  ensureLut(ranges.size(), params.angle_min, params.angle_increment);

  const bool with_intensity = !intensities.empty() && intensities.size() == ranges.size();
  const uint32_t point_step = with_intensity ? kXyziStep : kXyzStep;

  // The point bytes are newly generated (the wire holds polar ranges, not
  // cartesian points), so the buffer is owned and anchored — same pattern the
  // SDK codecs use for decoded payloads.
  auto owned = std::make_shared<std::vector<uint8_t>>();
  owned->reserve(ranges.size() * point_step);

  uint32_t kept = 0;
  for (size_t i = 0; i < ranges.size(); ++i) {
    const double range = static_cast<double>(ranges[i]);
    if (!std::isfinite(range)) {
      continue;
    }
    if (params.range_min.has_value() && range < *params.range_min) {
      continue;
    }
    if (params.range_max.has_value() && range > *params.range_max) {
      continue;
    }
    const float range_f = static_cast<float>(ranges[i]);
    const float point[4] = {
        range_f * cos_lut_[i], range_f * sin_lut_[i], 0.0f, with_intensity ? static_cast<float>(intensities[i]) : 0.0f};
    const auto* bytes = reinterpret_cast<const uint8_t*>(point);
    owned->insert(owned->end(), bytes, bytes + point_step);
    ++kept;
  }

  sdk::PointCloud cloud;
  cloud.width = kept;
  cloud.height = 1;
  cloud.point_step = point_step;
  cloud.row_step = kept * point_step;
  cloud.is_bigendian = false;
  cloud.is_dense = true;  // invalid rays were dropped, not NaN-filled
  cloud.fields = makeFields(with_intensity);
  cloud.data = Span<const uint8_t>(owned->data(), owned->size());
  cloud.anchor = std::move(owned);  // moving the shared_ptr does not move the bytes
  return cloud;
}

template sdk::PointCloud LaserScanProjector::projectImpl<float>(
    const ScanParams&, Span<const float>, Span<const float>);
template sdk::PointCloud LaserScanProjector::projectImpl<double>(
    const ScanParams&, Span<const double>, Span<const double>);

}  // namespace laser_scan
}  // namespace PJ
