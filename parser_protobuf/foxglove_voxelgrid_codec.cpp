// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#include "foxglove_voxelgrid_codec.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/coded_stream.h>

#include <limits>
#include <string>

#include "foxglove_descriptor_util.hpp"
#include "foxglove_wire_util.hpp"

namespace pj_protobuf {
namespace {

namespace gpio = google::protobuf::io;
using namespace pj_protobuf::wire;

using Datatype = PJ::sdk::PointField::Datatype;

/// Foxglove NumericType -> canonical PJ datatype. Swaps signed/unsigned relative
/// to ROS/SDK (UINT8=1 here vs INT8=1 there), identical to the PointCloud codec.
[[nodiscard]] Datatype mapFoxgloveNumericType(uint64_t t) {
  switch (t) {
    case 1:
      return Datatype::kUint8;
    case 2:
      return Datatype::kInt8;
    case 3:
      return Datatype::kUint16;
    case 4:
      return Datatype::kInt16;
    case 5:
      return Datatype::kUint32;
    case 6:
      return Datatype::kInt32;
    case 7:
      return Datatype::kFloat32;
    case 8:
      return Datatype::kFloat64;
    default:
      return Datatype::kUnknown;
  }
}

/// Parse one PackedElementField submessage of `len` bytes.
[[nodiscard]] bool readPackedElementField(
    gpio::CodedInputStream& in, uint32_t len, PJ::sdk::PointField& out, const VoxelGridFieldNumbers& fields) {
  const auto limit = in.PushLimit(static_cast<int>(len));
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (field == fields.pef_name && wt == kWireLen) {
      uint32_t s = 0;
      if (!in.ReadVarint32(&s) || !in.ReadString(&out.name, static_cast<int>(s))) {
        return false;
      }
    } else if (field == fields.pef_offset && wt == kWireI32) {
      uint32_t off = 0;
      if (!in.ReadLittleEndian32(&off)) {
        return false;
      }
      out.offset = off;
    } else if (field == fields.pef_type && wt == kWireVarint) {
      uint64_t t = 0;
      if (!in.ReadVarint64(&t)) {
        return false;
      }
      out.datatype = mapFoxgloveNumericType(t);
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  out.count = 1;  // Foxglove PackedElementField has no `count`; one element per field.
  return true;
}

}  // namespace

VoxelGridFieldNumbers resolveVoxelGridFieldNumbers(const google::protobuf::Descriptor* descriptor) {
  VoxelGridFieldNumbers n;  // official defaults
  n.timestamp = fieldNumberOr(descriptor, "timestamp", n.timestamp);
  n.frame_id = fieldNumberOr(descriptor, "frame_id", n.frame_id);
  n.pose = fieldNumberOr(descriptor, "pose", n.pose);
  n.row_count = fieldNumberOr(descriptor, "row_count", n.row_count);
  n.column_count = fieldNumberOr(descriptor, "column_count", n.column_count);
  n.cell_size = fieldNumberOr(descriptor, "cell_size", n.cell_size);
  n.slice_stride = fieldNumberOr(descriptor, "slice_stride", n.slice_stride);
  n.row_stride = fieldNumberOr(descriptor, "row_stride", n.row_stride);
  n.cell_stride = fieldNumberOr(descriptor, "cell_stride", n.cell_stride);
  n.fields = fieldNumberOr(descriptor, "fields", n.fields);
  n.data = fieldNumberOr(descriptor, "data", n.data);
  // nested PackedElementField, the message type of the `fields` field.
  const google::protobuf::Descriptor* pef = nestedDescriptor(descriptor, "fields");
  n.pef_name = fieldNumberOr(pef, "name", n.pef_name);
  n.pef_offset = fieldNumberOr(pef, "offset", n.pef_offset);
  n.pef_type = fieldNumberOr(pef, "type", n.pef_type);
  return n;
}

PJ::Expected<PJ::sdk::VoxelGrid> deserializeFoxgloveVoxelGridView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor, const VoxelGridFieldNumbers& fields) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.VoxelGrid: message too large"));
  }

  PJ::sdk::VoxelGrid grid;
  // Identity origin until a pose field overrides it (Foxglove may omit it).
  grid.origin.position = {.x = 0.0, .y = 0.0, .z = 0.0};
  grid.origin.orientation = {.x = 0.0, .y = 0.0, .z = 0.0, .w = 1.0};

  gpio::CodedInputStream in(data, static_cast<int>(size));
  in.SetTotalBytesLimit(std::numeric_limits<int>::max());

  int64_t ts_ns = 0;
  PJ::Span<const uint8_t> data_span;

  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    if (field == fields.timestamp) {
      if (wt != kWireLen) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: bad timestamp wire type"));
      }
      uint32_t len = 0;
      if (!in.ReadVarint32(&len) || !readTimestampNs(in, len, ts_ns)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read timestamp"));
      }
    } else if (field == fields.frame_id) {
      if (wt != kWireLen) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: bad frame_id wire type"));
      }
      uint32_t len = 0;
      if (!in.ReadVarint32(&len) || !in.ReadString(&grid.frame_id, static_cast<int>(len))) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read frame_id"));
      }
    } else if (field == fields.pose) {
      if (wt != kWireLen) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: bad pose wire type"));
      }
      uint32_t len = 0;
      if (!in.ReadVarint32(&len) || !readPose(in, len, grid.origin)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read pose"));
      }
    } else if (field == fields.cell_size) {
      if (wt != kWireLen) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: bad cell_size wire type"));
      }
      uint32_t len = 0;
      if (!in.ReadVarint32(&len) || !readVector3(in, len, grid.cell_size)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read cell_size"));
      }
    } else if (field == fields.row_count) {
      if (!readFixed32(in, wt, grid.row_count)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read row_count"));
      }
    } else if (field == fields.column_count) {
      if (!readFixed32(in, wt, grid.column_count)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read column_count"));
      }
    } else if (field == fields.slice_stride) {
      if (!readFixed32(in, wt, grid.slice_stride)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read slice_stride"));
      }
    } else if (field == fields.row_stride) {
      if (!readFixed32(in, wt, grid.row_stride)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read row_stride"));
      }
    } else if (field == fields.cell_stride) {
      if (!readFixed32(in, wt, grid.cell_stride)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read cell_stride"));
      }
    } else if (field == fields.fields) {
      if (wt != kWireLen) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: bad fields wire type"));
      }
      uint32_t len = 0;
      if (!in.ReadVarint32(&len)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read field length"));
      }
      PJ::sdk::PointField pf;
      if (!readPackedElementField(in, len, pf, fields)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read PackedElementField"));
      }
      grid.fields.push_back(std::move(pf));
    } else if (field == fields.data) {
      if (wt != kWireLen) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: bad data wire type"));
      }
      uint32_t len = 0;
      if (!in.ReadVarint32(&len)) {
        return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to read data length"));
      }
      if (len > 0) {
        // Zero-copy: alias the packed voxel bytes in place (a dense grid is
        // multi-MB). GetDirectBufferPointer hands back a pointer into the
        // original `data` buffer; the BufferAnchor keeps it alive past this call.
        const void* ptr = nullptr;
        int avail = 0;
        if (!in.GetDirectBufferPointer(&ptr, &avail) || avail < static_cast<int>(len)) {
          return PJ::unexpected(std::string("foxglove.VoxelGrid: data not contiguous"));
        }
        data_span = PJ::Span<const uint8_t>(static_cast<const uint8_t*>(ptr), len);
        if (!in.Skip(static_cast<int>(len))) {
          return PJ::unexpected(std::string("foxglove.VoxelGrid: failed to skip data"));
        }
      }
    } else if (!skipField(in, wt)) {
      return PJ::unexpected(std::string("foxglove.VoxelGrid: malformed message"));
    }
  }

  grid.timestamp_ns = ts_ns;
  // Foxglove omits the depth (slice) count; derive it from the buffer like the
  // PointCloud codec derives width from data.size() / point_stride.
  grid.slice_count = grid.slice_stride > 0 ? static_cast<uint32_t>(data_span.size() / grid.slice_stride) : 0;
  grid.data = data_span;
  grid.anchor = std::move(anchor);

  return grid;
}

}  // namespace pj_protobuf
