#pragma once
// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT
//
// Zero-copy decoder for the well-known `foxglove.Grid` protobuf schema into the
// canonical sdk::GridMap builtin object (plotjuggler_sdk 0.26.0). Same approach
// as foxglove_voxelgrid_codec.hpp: the wire layout is known, so we scan it
// directly with CodedInputStream and alias the packed cell bytes in place.
//
//   message foxglove.Grid {
//     google.protobuf.Timestamp timestamp = 1;
//     string                     frame_id  = 2;
//     foxglove.Pose              pose      = 3;   // -> sdk::GridMap.origin
//     fixed32                    column_count = 4;
//     foxglove.Vector2           cell_size = 5;
//     fixed32                    row_stride   = 6;
//     fixed32                    cell_stride  = 7;
//     repeated PackedElementField fields    = 8;
//     bytes                      data      = 9;
//   }
//   message PackedElementField { string name = 1; fixed32 offset = 2; NumericType type = 3; }
//
// foxglove.Grid carries no row count; it is derived as data.size() / row_stride
// by pj_grid_map::finalizeFoxgloveGrid, which also validates the layout.

#include <cstddef>
#include <cstdint>
#include <pj_base/buffer_anchor.hpp>
#include <pj_base/builtin/grid_map.hpp>
#include <pj_base/expected.hpp>

namespace google::protobuf {
class Descriptor;
}  // namespace google::protobuf

namespace pj_protobuf {

/// Field numbers for foxglove.Grid and its nested PackedElementField (pef_*),
/// resolved by NAME from the embedded descriptor (see foxglove_object_codecs.hpp
/// for the rationale). Defaults = official Foxglove numbering.
struct GridFieldNumbers {
  int timestamp = 1;
  int frame_id = 2;
  int pose = 3;
  int column_count = 4;
  int cell_size = 5;
  int row_stride = 6;
  int cell_stride = 7;
  int fields = 8;
  int data = 9;
  int pef_name = 1;
  int pef_offset = 2;
  int pef_type = 3;
};
[[nodiscard]] GridFieldNumbers resolveGridFieldNumbers(const google::protobuf::Descriptor* descriptor);

/// Decodes foxglove.Grid wire bytes into sdk::GridMap WITHOUT copying the packed
/// cell buffer: the returned grid's `data` span ALIASES `[data, data+size)` and
/// its `anchor` is the supplied one, which the caller keeps alive as long as the
/// grid is used. Rejects unknown PackedElementField types and any layout the
/// cell math could not index (finalizeFoxgloveGrid + validateGridMap).
[[nodiscard]] PJ::Expected<PJ::sdk::GridMap> deserializeFoxgloveGridView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor, const GridFieldNumbers& fields = {});

}  // namespace pj_protobuf
