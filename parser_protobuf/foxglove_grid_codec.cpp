// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#include "foxglove_grid_codec.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/coded_stream.h>

#include <limits>
#include <pj_grid_map/grid_map_transcoder.hpp>
#include <string>

#include "foxglove_descriptor_util.hpp"
#include "foxglove_wire_util.hpp"

namespace pj_protobuf {
namespace {

namespace gpio = google::protobuf::io;
using namespace pj_protobuf::wire;

/// Parse one PackedElementField submessage of `len` bytes.
[[nodiscard]] bool readPackedElementField(
    gpio::CodedInputStream& in, uint32_t len, PJ::sdk::PointField& out, const GridFieldNumbers& fields) {
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
      out.datatype = PJ::grid_map::mapFoxglovePackedElementType(t);
    } else if (!skipField(in, wt)) {
      return false;
    }
  }
  in.PopLimit(limit);
  out.count = 1;  // Foxglove PackedElementField has no `count`; one element per field.
  return true;
}

}  // namespace

GridFieldNumbers resolveGridFieldNumbers(const google::protobuf::Descriptor* descriptor) {
  GridFieldNumbers n;  // official defaults
  n.timestamp = fieldNumberOr(descriptor, "timestamp", n.timestamp);
  n.frame_id = fieldNumberOr(descriptor, "frame_id", n.frame_id);
  n.pose = fieldNumberOr(descriptor, "pose", n.pose);
  n.column_count = fieldNumberOr(descriptor, "column_count", n.column_count);
  n.cell_size = fieldNumberOr(descriptor, "cell_size", n.cell_size);
  n.row_stride = fieldNumberOr(descriptor, "row_stride", n.row_stride);
  n.cell_stride = fieldNumberOr(descriptor, "cell_stride", n.cell_stride);
  n.fields = fieldNumberOr(descriptor, "fields", n.fields);
  n.data = fieldNumberOr(descriptor, "data", n.data);
  const google::protobuf::Descriptor* pef = nestedDescriptor(descriptor, "fields");
  n.pef_name = fieldNumberOr(pef, "name", n.pef_name);
  n.pef_offset = fieldNumberOr(pef, "offset", n.pef_offset);
  n.pef_type = fieldNumberOr(pef, "type", n.pef_type);
  return n;
}

PJ::Expected<PJ::sdk::GridMap> deserializeFoxgloveGridView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor, const GridFieldNumbers& fields) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.Grid: message too large"));
  }

  PJ::sdk::GridMap grid;  // origin defaults to identity when the pose is omitted

  gpio::CodedInputStream in(data, static_cast<int>(size));
  in.SetTotalBytesLimit(std::numeric_limits<int>::max());

  const auto fail = [](const char* what) { return PJ::unexpected(std::string("foxglove.Grid: ") + what); };
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int field = static_cast<int>(tag >> 3);
    const uint32_t wt = tag & 0x7u;
    uint32_t len = 0;
    if (field == fields.timestamp) {
      if (wt != kWireLen || !in.ReadVarint32(&len) || !readTimestampNs(in, len, grid.timestamp_ns)) {
        return fail("failed to read timestamp");
      }
    } else if (field == fields.frame_id) {
      if (wt != kWireLen || !in.ReadVarint32(&len) || !in.ReadString(&grid.frame_id, static_cast<int>(len))) {
        return fail("failed to read frame_id");
      }
    } else if (field == fields.pose) {
      if (wt != kWireLen || !in.ReadVarint32(&len) || !readPose(in, len, grid.origin)) {
        return fail("failed to read pose");
      }
    } else if (field == fields.cell_size) {
      if (wt != kWireLen || !in.ReadVarint32(&len) || !readVector2(in, len, grid.cell_size)) {
        return fail("failed to read cell_size");
      }
    } else if (field == fields.column_count) {
      if (!readFixed32(in, wt, grid.column_count)) {
        return fail("failed to read column_count");
      }
    } else if (field == fields.row_stride) {
      if (!readFixed32(in, wt, grid.row_stride)) {
        return fail("failed to read row_stride");
      }
    } else if (field == fields.cell_stride) {
      if (!readFixed32(in, wt, grid.cell_stride)) {
        return fail("failed to read cell_stride");
      }
    } else if (field == fields.fields) {
      PJ::sdk::PointField pf;
      if (wt != kWireLen || !in.ReadVarint32(&len) || !readPackedElementField(in, len, pf, fields)) {
        return fail("failed to read PackedElementField");
      }
      grid.fields.push_back(std::move(pf));
    } else if (field == fields.data) {
      if (wt != kWireLen || !in.ReadVarint32(&len)) {
        return fail("failed to read data length");
      }
      if (len > 0) {
        // Zero-copy: alias the packed cell bytes in place; the BufferAnchor keeps
        // the original buffer alive past this call.
        const void* ptr = nullptr;
        int avail = 0;
        if (!in.GetDirectBufferPointer(&ptr, &avail) || avail < static_cast<int>(len)) {
          return fail("data not contiguous (truncated message)");
        }
        grid.data = PJ::Span<const uint8_t>(static_cast<const uint8_t*>(ptr), len);
        if (!in.Skip(static_cast<int>(len))) {
          return fail("failed to skip data");
        }
      }
    } else if (!skipField(in, wt)) {
      return fail("malformed message");
    }
  }

  grid.anchor = std::move(anchor);
  if (auto ok = PJ::grid_map::finalizeFoxgloveGrid(grid); !ok) {
    return PJ::unexpected(std::string("foxglove.") + std::move(ok).error());
  }
  return grid;
}

}  // namespace pj_protobuf
