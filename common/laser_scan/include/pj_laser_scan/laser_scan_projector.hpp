// pj_laser_scan — planar laser-scan -> point-cloud projection shared by the
// message parsers (parser_ros: sensor_msgs/LaserScan, parser_protobuf:
// foxglove.LaserScan). Qt-free; depends only on pj_base vocabulary types.
//
// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <pj_base/builtin/point_cloud.hpp>
#include <pj_base/span.hpp>
#include <vector>

namespace PJ {
namespace laser_scan {

/// Per-message scan geometry and validity bounds.
///
/// Ray `i` is projected at `theta = angle_min + i * angle_increment` (radians,
/// counterclockwise around +Z, 0 = +X — the ROS / Foxglove convention).
struct ScanParams {
  double angle_min = 0.0;        ///< Bearing of ray 0 [rad].
  double angle_increment = 0.0;  ///< Angular distance between consecutive rays [rad].
  /// Inclusive validity bounds [m]. A ray whose range falls outside
  /// [range_min, range_max] is dropped. Leave unset when the source schema
  /// carries no bounds (foxglove.LaserScan): then only non-finite ranges drop.
  std::optional<double> range_min = std::nullopt;
  std::optional<double> range_max = std::nullopt;
};

/// Projects planar laser scans into unorganized, dense `sdk::PointCloud`s
/// (height = 1, width = number of kept rays; fields x/y/z FLOAT32 at offsets
/// 0/4/8, plus intensity FLOAT32 at 12 when intensities pass through).
///
/// Stateful on purpose: the per-ray cos/sin lookup table is cached on the key
/// `(ray_count, angle_min, angle_increment)` and recomputed ONLY when that key
/// changes — for a fixed scanner configuration the trig runs once per
/// recording, not once per message. Keep one projector per parser instance
/// (i.e. per topic) so the cache actually sticks.
///
/// Not thread-safe: callers serialize project() calls per instance, the same
/// contract every message-parser handler already follows.
class LaserScanProjector {
 public:
  /// Projects one scan. Rays with a non-finite range, or a range outside the
  /// inclusive [range_min, range_max] bounds (when provided), are dropped —
  /// the output is dense (`is_dense = true`) and `width` is the kept count.
  ///
  /// Intensities pass through as a fourth FLOAT32 channel only when
  /// `intensities.size() == ranges.size()`; otherwise (absent or mismatched)
  /// the layout is xyz-only. A kept ray keeps the intensity of its original
  /// index.
  ///
  /// The returned cloud's `data` bytes are NEWLY GENERATED and owned: `anchor`
  /// holds the backing buffer, so the cloud outlives both the wire payload and
  /// this projector. `frame_id` / `timestamp_ns` are left default — the caller
  /// stamps them from the message envelope.
  [[nodiscard]] sdk::PointCloud project(
      const ScanParams& params, Span<const float> ranges, Span<const float> intensities);

  /// Overload for sources that carry ranges/intensities as doubles
  /// (foxglove.LaserScan). Values are narrowed to float at projection time —
  /// the output layout is identical to the float overload.
  [[nodiscard]] sdk::PointCloud project(
      const ScanParams& params, Span<const double> ranges, Span<const double> intensities);

  /// Number of times the cos/sin LUT has been recomputed. Exposed so tests can
  /// assert cache reuse/invalidation; not meaningful for production callers.
  [[nodiscard]] size_t lutRebuildCount() const {
    return lut_rebuild_count_;
  }

 private:
  template <typename Scalar>
  sdk::PointCloud projectImpl(const ScanParams& params, Span<const Scalar> ranges, Span<const Scalar> intensities);

  /// Recomputes cos_lut_/sin_lut_ iff (ray_count, angle_min, angle_increment)
  /// differs from the cached key. Angles are equality-compared; NaN params
  /// never match, so degenerate scans rebuild the LUT each message (safe,
  /// just uncached). A fixed scanner config reproduces identical doubles
  /// message after message, so the cache holds on the common path.
  void ensureLut(size_t ray_count, double angle_min, double angle_increment);

  // Cached LUT key. The NaN initializers are an unmatchable sentinel (NaN
  // never compares equal), so the first project() always builds the LUT —
  // no separate "valid" flag needed.
  size_t lut_ray_count_ = 0;
  double lut_angle_min_ = std::numeric_limits<double>::quiet_NaN();
  double lut_angle_increment_ = std::numeric_limits<double>::quiet_NaN();
  size_t lut_rebuild_count_ = 0;
  std::vector<float> cos_lut_;
  std::vector<float> sin_lut_;
};

}  // namespace laser_scan
}  // namespace PJ
