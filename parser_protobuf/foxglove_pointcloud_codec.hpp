#pragma once
// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT
//
// Zero-copy decoder for the well-known `foxglove.PointCloud` protobuf schema
// into the canonical sdk::PointCloud builtin object. Mirrors what parser_ros
// does for sensor_msgs/PointCloud2 (RosParser::parsePointCloud), but reads the
// Foxglove protobuf wire layout instead of CDR.
//
// The Foxglove schema differs from both ROS PointCloud2 and the canonical
// PJ.PointCloud, so the SDK's deserializePointCloud() does NOT apply — this is
// a Foxglove-specific reader, hence it lives plugin-local:
//
//   message foxglove.PointCloud {
//     google.protobuf.Timestamp timestamp = 1;
//     string                     frame_id  = 2;
//     foxglove.Pose              pose      = 3;   // dropped (sdk::PointCloud has no pose)
//     fixed32                    point_stride = 4;
//     repeated PackedElementField fields   = 5;
//     bytes                      data      = 6;
//   }
//   message PackedElementField { string name = 1; fixed32 offset = 2; NumericType type = 3; }
//   enum NumericType { UNKNOWN=0, UINT8=1, INT8=2, UINT16=3, INT16=4, UINT32=5, INT32=6, FLOAT32=7, FLOAT64=8 };
//
// Note the NumericType enum swaps signed/unsigned relative to ROS/SDK
// (UINT8=1 vs INT8=1), so a dedicated remap is required.

#include <cstddef>
#include <cstdint>
#include <pj_base/buffer_anchor.hpp>
#include <pj_base/builtin/point_cloud.hpp>
#include <pj_base/expected.hpp>
#include <pj_laser_scan/laser_scan_projector.hpp>
#include <string>

namespace pj_protobuf {

/// Result of decoding a foxglove.PointCloud message. The `pose` field has no
/// home in sdk::PointCloud (which expresses geometry in `frame_id` and relies
/// on an external TF tree, exactly like sensor_msgs/PointCloud2); we surface
/// its presence so the caller can warn when a non-identity pose is silently
/// dropped.
struct FoxglovePointCloudDecode {
  PJ::sdk::PointCloud cloud;
  bool has_pose = false;         ///< field 3 (pose) was present on the wire.
  bool pose_is_identity = true;  ///< pose == identity (zero translation, unit quaternion).
};

/// Decodes foxglove.PointCloud wire bytes into sdk::PointCloud WITHOUT copying
/// the packed point buffer. The returned cloud's `data` span ALIASES the input
/// `[data, data+size)` and its `anchor` is set to the supplied `anchor`, which
/// the caller must keep alive for as long as the cloud (and its `data` span) is
/// used. `width`/`height`/`row_step`/`is_bigendian`/`is_dense` are synthesized
/// (Foxglove omits them: it is always a flat, little-endian, dense point list).
[[nodiscard]] PJ::Expected<FoxglovePointCloudDecode> deserializeFoxglovePointCloudView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor);

/// Result of decoding a foxglove.LaserScan message into an eagerly projected
/// point cloud. As with PointCloud above, the inline `pose` has no home in
/// sdk::PointCloud, so its presence/identity is surfaced for the caller's
/// warn-once policy.
///
///   message foxglove.LaserScan {
///     google.protobuf.Timestamp timestamp = 1;
///     string          frame_id    = 2;
///     foxglove.Pose   pose        = 3;   // dropped (sdk::PointCloud has no pose)
///     double          start_angle = 4;
///     double          end_angle   = 5;
///     repeated double ranges      = 6;   // packed
///     repeated double intensities = 7;   // packed
///   }
struct FoxgloveLaserScanDecode {
  PJ::sdk::PointCloud cloud;     ///< Projected x/y/z(/intensity) FLOAT32 cloud; data is OWNED (anchored).
  double start_angle = 0.0;      ///< Bearing of ray 0 [rad].
  double end_angle = 0.0;        ///< Bearing of the last ray [rad].
  uint64_t ray_count = 0;        ///< Rays on the wire (before invalid rays are dropped).
  bool has_pose = false;         ///< field 3 (pose) was present on the wire.
  bool pose_is_identity = true;  ///< pose == identity (zero translation, unit quaternion).
};

/// Decodes foxglove.LaserScan wire bytes and eagerly projects them through
/// `projector` (rays at equally-spaced angles between start_angle and
/// end_angle; angle_increment = (end-start)/(N-1) for N > 1). Unlike the
/// PointCloud view decoder there is NO zero-copy and no anchor parameter: the
/// point bytes are newly generated and owned by the returned cloud's anchor.
/// Foxglove carries no range bounds, so only non-finite ranges are dropped.
/// The caller keeps `projector` alive across messages so its cos/sin LUT is
/// reused for a fixed scanner configuration.
[[nodiscard]] PJ::Expected<FoxgloveLaserScanDecode> deserializeFoxgloveLaserScan(
    const uint8_t* data, size_t size, PJ::laser_scan::LaserScanProjector& projector);

/// Slim foxglove.LaserScan metadata for the scalar route.
struct FoxgloveLaserScanInfo {
  int64_t timestamp_ns = 0;  ///< 0 when the message carries no timestamp.
  std::string frame_id;
  double start_angle = 0.0;
  double end_angle = 0.0;
  uint64_t num_ranges = 0;  ///< Rays on the wire (packed LEN/8, plus any unpacked records).
};

/// Header-only walk of a foxglove.LaserScan: reads timestamp, frame_id and the
/// angles, and derives `num_ranges` from the LEN of the packed `ranges` field —
/// no LUT, no cartesian projection, no ranges materialization. Use this for
/// per-message scalar metadata; the O(N) projection stays on the object route.
[[nodiscard]] PJ::Expected<FoxgloveLaserScanInfo> readFoxgloveLaserScanInfo(const uint8_t* data, size_t size);

}  // namespace pj_protobuf
