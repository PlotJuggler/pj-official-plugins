#pragma once
// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT
//
// Zero-copy decoder for the well-known `foxglove.VoxelGrid` protobuf schema into
// the canonical sdk::VoxelGrid builtin object (the dense 3D voxel grid added in
// plotjuggler_sdk 0.10.0). Same approach as foxglove_pointcloud_codec.hpp: the
// wire layout is known, so we scan it directly with CodedInputStream and alias
// the packed voxel bytes in place.
//
//   message foxglove.VoxelGrid {
//     google.protobuf.Timestamp timestamp = 1;
//     string                     frame_id  = 2;
//     foxglove.Pose              pose      = 3;   // -> sdk::VoxelGrid.origin
//     fixed32                    row_count = 4;
//     fixed32                    column_count = 5;
//     foxglove.Vector3           cell_size = 6;
//     fixed32                    slice_stride = 7;
//     fixed32                    row_stride   = 8;
//     fixed32                    cell_stride  = 9;
//     repeated PackedElementField fields    = 10;
//     bytes                      data      = 11;
//   }
//   message PackedElementField { string name = 1; fixed32 offset = 2; NumericType type = 3; }
//
// The PackedElementField NumericType enum swaps signed/unsigned relative to
// ROS/SDK (UINT8=1, INT8=2), identical to foxglove.PointCloud, so the same remap
// applies. foxglove.VoxelGrid carries no explicit slice/depth count; it is
// derived as data.size() / slice_stride (mirroring how the PointCloud codec
// derives `width` from data.size() / point_stride).

#include <cstddef>
#include <cstdint>
#include <pj_base/buffer_anchor.hpp>
#include <pj_base/builtin/voxel_grid.hpp>
#include <pj_base/expected.hpp>

namespace google::protobuf {
class Descriptor;
}  // namespace google::protobuf

namespace pj_protobuf {

// Descriptor-driven field numbers (see foxglove_object_codecs.hpp for the full
// rationale): a self-describing .mcap may embed a schema that renumbers these,
// so resolve them by NAME from the embedded descriptor. Defaults = official
// Foxglove numbering, so schemaless streams behave exactly as before.

/// Field numbers for foxglove.VoxelGrid and its nested PackedElementField (pef_*).
struct VoxelGridFieldNumbers {
  int timestamp = 1;
  int frame_id = 2;
  int pose = 3;
  int row_count = 4;
  int column_count = 5;
  int cell_size = 6;
  int slice_stride = 7;
  int row_stride = 8;
  int cell_stride = 9;
  int fields = 10;
  int data = 11;
  // nested PackedElementField { name=1, offset=2, type=3 }
  int pef_name = 1;
  int pef_offset = 2;
  int pef_type = 3;
};
[[nodiscard]] VoxelGridFieldNumbers resolveVoxelGridFieldNumbers(const google::protobuf::Descriptor* descriptor);

/// Decodes foxglove.VoxelGrid wire bytes into sdk::VoxelGrid WITHOUT copying the
/// packed voxel buffer. The returned grid's `data` span ALIASES the input
/// `[data, data+size)` and its `anchor` is set to the supplied `anchor`, which
/// the caller must keep alive for as long as the grid (and its `data` span) is
/// used. `slice_count` is derived (Foxglove omits it: data.size() / slice_stride).
[[nodiscard]] PJ::Expected<PJ::sdk::VoxelGrid> deserializeFoxgloveVoxelGridView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor, const VoxelGridFieldNumbers& fields = {});

}  // namespace pj_protobuf
